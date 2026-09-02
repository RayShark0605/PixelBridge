#!/usr/bin/env python3
"""Verify and seal a user-authorized single-monitor RemoteVisual file pilot.

This verifier intentionally keeps the recovered-file truth separate from the
formal dual-monitor PilotPlan provenance. It reuses the production endpoint
report merger, validates both Decoder journals and the offline Replay result,
and publishes one create-only JSON verification record.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
from typing import Any, Iterable

import pb_remote_visual_report as report_tool


MAX_JOURNAL_BYTES = 16 * 1024 * 1024
MAX_JOURNAL_LINE_BYTES = 256 * 1024
MAX_TERMINAL_JOURNAL_DELAY_MILLISECONDS = 5_000
RUN_ID_PATTERN = re.compile(r"^[0-9a-f]{32}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")


class PilotVerificationError(ValueError):
    pass


def _fail(message: str) -> None:
    raise PilotVerificationError(message)


def _require_dict(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(f"{name} must be a JSON object")
    return value


def _require_list(value: Any, name: str) -> list[Any]:
    if not isinstance(value, list):
        _fail(f"{name} must be a JSON array")
    return value


def _require_string(value: Any, name: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or (not allow_empty and not value):
        _fail(f"{name} must be a {'possibly empty ' if allow_empty else ''}string")
    return value


def _require_bool(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        _fail(f"{name} must be a boolean")
    return value


def _require_integer(value: Any, name: str, *, minimum: int | None = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        _fail(f"{name} must be an integer")
    if minimum is not None and value < minimum:
        _fail(f"{name} must be at least {minimum}")
    return value


def _require_number(value: Any, name: str, *, minimum: float | None = None,
                    maximum: float | None = None) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        _fail(f"{name} must be a finite number")
    result = float(value)
    if minimum is not None and result < minimum:
        _fail(f"{name} must be at least {minimum}")
    if maximum is not None and result > maximum:
        _fail(f"{name} must be at most {maximum}")
    return result


def _expect_equal(actual: Any, expected: Any, name: str) -> None:
    if actual != expected:
        _fail(f"{name} mismatch: {actual!r} != {expected!r}")


def _load_json(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    try:
        return report_tool._load_report_and_seal(path)
    except report_tool.ReportError as error:
        raise PilotVerificationError(str(error)) from error


def _seal(path: Path) -> dict[str, Any]:
    try:
        return report_tool._hash_file(path)
    except report_tool.ReportError as error:
        raise PilotVerificationError(str(error)) from error


def _read_stable_bytes(path: Path, maximum_bytes: int, name: str) -> tuple[bytes, dict[str, Any]]:
    """Read and seal one bounded file without comparing synthetic Windows execute bits.

    On Windows, pathlib.stat() may synthesize execute permission bits from the
    filename suffix while os.fstat() reports a different st_mode for the same
    open file. File identity, size and modification time remain authoritative;
    both observations are still required to be regular files.
    """
    try:
        resolved = path.resolve(strict=True)
        initial = resolved.stat()
        if not stat.S_ISREG(initial.st_mode):
            _fail(f"{name} is not a regular file: {path}")
        with resolved.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat.S_ISREG(before.st_mode):
                _fail(f"{name} is not a regular file: {path}")
            initial_identity = (initial.st_dev, initial.st_ino, initial.st_size, initial.st_mtime_ns)
            open_identity = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            if initial_identity != open_identity:
                _fail(f"{name} changed or was replaced while being opened: {path}")
            if before.st_size <= 0 or before.st_size > maximum_bytes:
                _fail(f"{name} size is outside 1..{maximum_bytes} bytes")
            raw = stream.read(maximum_bytes + 1)
            after = os.fstat(stream.fileno())
        current = resolved.stat()
        after_identity = (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
        current_identity = (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns)
        if open_identity != after_identity or open_identity != current_identity or len(raw) != before.st_size:
            _fail(f"{name} changed while being read and sealed: {path}")
        return raw, {"path": str(resolved), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    except PilotVerificationError:
        raise
    except (OSError, RuntimeError) as error:
        raise PilotVerificationError(f"cannot read and seal {name} {path}: {error}") from error


def _seal_windows_executable(path: Path) -> dict[str, Any]:
    _, identity = _read_stable_bytes(path, 256 * 1024 * 1024, "executable")
    return identity


def _read_exact_text(path: Path, maximum_bytes: int, name: str) -> tuple[str, dict[str, Any]]:
    raw, identity = _read_stable_bytes(path, maximum_bytes, name)
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise PilotVerificationError(f"cannot read {name}: {error}") from error
    return text, identity


def _validate_exit_code(path: Path, name: str) -> dict[str, Any]:
    text, identity = _read_exact_text(path, 16, name)
    if text not in ("0", "0\n", "0\r\n"):
        _fail(f"{name} is not exact exit code 0")
    return identity


def _validate_used_run_id(path: Path, expected_run_id: str) -> dict[str, Any]:
    text, identity = _read_exact_text(path, 64, "used RunId")
    if text != expected_run_id or identity["bytes"] != 32:
        _fail("used RunId must be the exact 32-byte shared RunId without whitespace")
    return identity


def _strict_journal_value(line: str, name: str) -> dict[str, Any]:
    try:
        value = json.loads(line, object_pairs_hook=report_tool._strict_object,
                           parse_constant=report_tool._reject_constant)
        report_tool._validate_tree(value)
    except (json.JSONDecodeError, report_tool.ReportError) as error:
        raise PilotVerificationError(f"{name} is malformed or ambiguous: {error}") from error
    return _require_dict(value, name)


def _validate_journal(path: Path, report: dict[str, Any], role: str, *,
                      require_single_monitor_fullscreen: bool) -> tuple[dict[str, Any], dict[str, Any]]:
    text, identity = _read_exact_text(path, MAX_JOURNAL_BYTES, f"{role} journal")
    if "\r" in text.replace("\r\n", ""):
        _fail(f"{role} journal contains an isolated carriage return")
    lines = text.splitlines()
    if not lines:
        _fail(f"{role} journal is empty")
    records: list[dict[str, Any]] = []
    previous_unix_ms: int | None = None
    monotonic_fields = (
        ("broadcastRuntimeMs", "frameSequence", "logicalDwellViolationCount", "sourceTextureReplacements",
         "repeatedPresentCalls", "monitorSafetyRevalidationCount")
        if role == "Encoder" else
        ("verifiedRawBytes", "verifiedEncodedBytes", "captureArrivedFrames", "captureDeliveredFrames",
         "bootstrapAttempts", "bootstrapSuccesses", "evaluatedCodewords", "fecFailures", "crcFailures",
         "temporallyAdmittedTransportBlocks", "acceptedTransportBlocks", "outerUniqueSymbols",
         "outerResourceRejections", "outerConflictRejections", "replayWrittenFrames", "replayDroppedFrames")
    )
    previous_counters: dict[str, int] = {}
    for index, line in enumerate(lines, start=1):
        if len(line.encode("utf-8")) > MAX_JOURNAL_LINE_BYTES:
            _fail(f"{role} journal line {index} exceeds the line limit")
        record = _strict_journal_value(line, f"{role} journal line {index}")
        _expect_equal(record.get("schema"), "PixelBridge.RunJournal.1", f"{role} journal schema")
        _expect_equal(record.get("role"), role, f"{role} journal role")
        _expect_equal(record.get("runId"), report.get("runId"), f"{role} journal RunId")
        unix_ms = _require_integer(record.get("unixMs"), f"{role} journal unixMs", minimum=1)
        if previous_unix_ms is not None and unix_ms < previous_unix_ms:
            _fail(f"{role} journal timestamps are not monotonic")
        previous_unix_ms = unix_ms
        _require_string(record.get("state"), f"{role} journal state")
        if require_single_monitor_fullscreen:
            _expect_equal(record.get("singleMonitorFullscreen"), True,
                          "Encoder journal single-monitor authority")
            _expect_equal(record.get("monitorSafetyPreflightPassed"), False,
                          "Encoder journal formal monitor preflight")
        for field in monotonic_fields:
            value = _require_integer(record.get(field), f"{role} journal {field}", minimum=0)
            if field in previous_counters and value < previous_counters[field]:
                _fail(f"{role} journal counter {field} is not monotonic")
            previous_counters[field] = value
        records.append(record)

    evidence = _require_dict(report.get("evidence"), f"{role} report evidence")
    _expect_equal(evidence.get("journalEnabled"), True, f"{role} journal enabled")
    _expect_equal(evidence.get("valid"), True, f"{role} journal evidence valid")
    _expect_equal(evidence.get("journalTruncated"), False, f"{role} journal truncated")
    _expect_equal(evidence.get("journalFinished"), True, f"{role} journal finished")
    _expect_equal(evidence.get("journalSamples"), len(records), f"{role} journal sample count")
    _expect_equal(evidence.get("journalBytes"), identity["bytes"], f"{role} journal byte count")
    _expect_equal(evidence.get("invalidReason"), "", f"{role} journal invalid reason")
    started = _require_integer(report.get("runStartedUnixMilliseconds"), f"{role} run start", minimum=1)
    ended = _require_integer(report.get("runEndedUnixMilliseconds"), f"{role} run end", minimum=started)
    if records[0]["unixMs"] < started or records[0]["unixMs"] > started + 5_000:
        _fail(f"{role} journal does not start with the endpoint run")
    if records[-1]["unixMs"] < ended or records[-1]["unixMs"] > ended + MAX_TERMINAL_JOURNAL_DELAY_MILLISECONDS:
        _fail(f"{role} terminal journal timestamp is outside the bounded finalization delay")
    _expect_equal(records[-1].get("state"), report.get("state"), f"{role} terminal state")

    if role == "Encoder":
        terminal_map = {
            "broadcastRuntimeMs": "broadcastRuntimeMilliseconds",
            "frameSequence": "frameSequence",
            "logicalDwellViolationCount": "logicalDwellViolationCount",
            "sourceTextureReplacements": "sourceTextureReplacements",
            "repeatedPresentCalls": "repeatedPresentCalls",
            "monitorSafetyRevalidationCount": ("monitorSafety", "revalidationCount"),
        }
    else:
        terminal_map = {
            "verifiedRawBytes": "verifiedRawBytes",
            "verifiedEncodedBytes": "verifiedEncodedBytes",
            "captureArrivedFrames": "captureArrivedFrames",
            "captureDeliveredFrames": "captureDeliveredFrames",
            "evaluatedCodewords": "evaluatedCodewords",
            "fecFailures": "fecFailures",
            "crcFailures": "crcFailures",
            "temporallyAdmittedTransportBlocks": "temporallyAdmittedTransportBlocks",
            "acceptedTransportBlocks": "acceptedTransportBlocks",
            "outerUniqueSymbols": ("outerAdmission", "uniqueSymbols"),
            "outerResourceRejections": ("outerAdmission", "resourceRejections"),
            "outerConflictRejections": ("outerAdmission", "conflictRejections"),
        }
    terminal = records[-1]
    for journal_name, report_name in terminal_map.items():
        if isinstance(report_name, tuple):
            report_value = _require_dict(report.get(report_name[0]), f"{role} report {report_name[0]}").get(report_name[1])
        else:
            report_value = report.get(report_name)
        _expect_equal(terminal.get(journal_name), report_value, f"{role} terminal {journal_name}")
    if role == "Decoder":
        _expect_equal(terminal.get("wholeFileDigestPass"), report.get("wholeFileDigestVerified"),
                      "Decoder terminal WholeFileDigest")
        _expect_equal(terminal.get("finalPublishPass"), report.get("finalPublishSucceeded"),
                      "Decoder terminal publish")
    return identity, {"samples": len(records), "first": records[0], "last": terminal}


def _find_file_entry(manifest: dict[str, Any], relative_path: str) -> dict[str, Any]:
    matches = [entry for entry in _require_list(manifest.get("files"), "delivery files")
               if isinstance(entry, dict) and entry.get("path") == relative_path]
    if len(matches) != 1:
        _fail(f"delivery manifest must contain exactly one {relative_path}")
    return matches[0]


def _validate_output_directory(output_path: Path, name: str) -> None:
    parent = output_path.parent.resolve(strict=True)
    candidate = parent / output_path.name
    if candidate.is_symlink() or not stat.S_ISREG(candidate.lstat().st_mode):
        _fail(f"{name} published path must be a non-symlink regular file")
    resolved = candidate.resolve(strict=True)
    entries = list(parent.iterdir())
    if len(entries) != 1 or entries[0].is_symlink() or not stat.S_ISREG(entries[0].lstat().st_mode) or \
            entries[0].resolve(strict=True) != resolved:
        _fail(f"{name} directory must contain exactly the one published file")


def _verify(arguments: argparse.Namespace) -> dict[str, Any]:
    if not RUN_ID_PATTERN.fullmatch(arguments.expected_run_id):
        _fail("expected RunId must be 32 lowercase hexadecimal characters")
    if not COMMIT_PATTERN.fullmatch(arguments.expected_git_commit):
        _fail("expected git commit must be 40 lowercase hexadecimal characters")

    encoder, encoder_identity = _load_json(arguments.encoder_report)
    live, live_identity = _load_json(arguments.live_decoder_report)
    offline, offline_identity = _load_json(arguments.offline_decoder_report)
    try:
        report_tool._validate_v2_endpoints(encoder, live)
        report_tool._validate_v2_endpoints(encoder, offline)
    except report_tool.ReportError as error:
        raise PilotVerificationError(str(error)) from error

    for name, endpoint, role in (("Encoder", encoder, "Encoder"), ("live Decoder", live, "Decoder"),
                                 ("offline Decoder", offline, "Decoder")):
        _expect_equal(endpoint.get("schema"), "PixelBridge.RunReport.2", f"{name} schema")
        _expect_equal(endpoint.get("role"), role, f"{name} role")
        _expect_equal(endpoint.get("runId"), arguments.expected_run_id, f"{name} RunId")
        _expect_equal(endpoint.get("gitCommit"), arguments.expected_git_commit, f"{name} git commit")
        _expect_equal(endpoint.get("profile"), "PB-RemoteVisual-LF4-X1 (Experimental)", f"{name} profile")

    _expect_equal(encoder.get("state"), "Stopped", "Encoder terminal state")
    _expect_equal(encoder.get("sourceStable"), True, "Encoder source stability")
    _expect_equal(encoder.get("candidateContractSatisfied"), False, "single-monitor formal candidate contract")
    _expect_equal(encoder.get("configuredLogicalVisualFps"), 2, "Encoder logical FPS")
    generated_fps = _require_number(encoder.get("generatedVisualFramesPerSecond"), "generated visual FPS",
                                    minimum=0.0, maximum=5.0)
    configured_dwell = _require_integer(encoder.get("configuredLogicalDwellMilliseconds"),
                                        "configured logical dwell", minimum=200)
    minimum_dwell = _require_number(encoder.get("minimumObservedLogicalDwellMilliseconds"),
                                    "minimum observed dwell", minimum=float(configured_dwell))
    _expect_equal(encoder.get("logicalDwellViolationCount"), 0, "Encoder dwell violations")
    status_message = _require_string(encoder.get("statusMessage"), "Encoder status message")
    if "stopped by user" not in status_message or "no sender-side receiver completion was inferred" not in status_message:
        _fail("Encoder terminal status does not preserve manual-stop/no-completion semantics")
    data_window = _require_dict(encoder.get("dataWindow"), "Encoder dataWindow")
    _expect_equal(data_window.get("singleMonitorFullscreen"), True, "Encoder single-monitor report flag")
    _require_integer(data_window.get("width"), "Encoder surface width", minimum=1920)
    _require_integer(data_window.get("height"), "Encoder surface height", minimum=1080)
    sender_monitor = _require_dict(encoder.get("monitorSafety"), "Encoder monitorSafety")
    _expect_equal(sender_monitor.get("preflightPassed"), False, "single-monitor formal preflight")
    _expect_equal(sender_monitor.get("status"), "NotApplicableSingleMonitorFullscreen",
                  "single-monitor preflight status")

    _expect_equal(live.get("state"), "Completed", "live Decoder state")
    _expect_equal(live.get("requestedBackend"), "WGC", "live requested backend")
    _expect_equal(live.get("actualBackend"), "WGC", "live actual backend")
    _expect_equal(live.get("wholeFileDigestVerified"), True, "live WholeFileDigest")
    _expect_equal(live.get("finalPublishSucceeded"), True, "live publish")
    live_monitor = _require_dict(live.get("monitorSafety"), "live monitorSafety")
    _expect_equal(live_monitor.get("preflightPassed"), True, "live monitor preflight")
    _expect_equal(live_monitor.get("status"), "PASS", "live monitor status")
    live_outer = _require_dict(live.get("outerAdmission"), "live outerAdmission")
    _expect_equal(live_outer.get("resourceRejections"), 0, "live Outer resource rejection")
    _expect_equal(live_outer.get("conflictRejections"), 0, "live Outer conflict rejection")
    _expect_equal(live.get("crcFailures"), 0, "live CRC failures")
    _expect_equal(live.get("identityFailures"), 0, "live identity failures")

    _expect_equal(offline.get("state"), "Completed", "offline Decoder state")
    _expect_equal(offline.get("actualBackend"), None, "offline live backend")
    _expect_equal(offline.get("wholeFileDigestVerified"), True, "offline WholeFileDigest")
    _expect_equal(offline.get("finalPublishSucceeded"), True, "offline publish")
    offline_monitor = _require_dict(offline.get("monitorSafety"), "offline monitorSafety")
    _expect_equal(offline_monitor.get("preflightPassed"), False, "offline monitor preflight")
    _expect_equal(offline_monitor.get("status"), "NotApplicableOfflineReplay", "offline monitor status")
    offline_outer = _require_dict(offline.get("outerAdmission"), "offline outerAdmission")
    _expect_equal(offline_outer.get("resourceRejections"), 0, "offline Outer resource rejection")
    _expect_equal(offline_outer.get("conflictRejections"), 0, "offline Outer conflict rejection")
    _expect_equal(offline.get("crcFailures"), 0, "offline CRC failures")
    _expect_equal(offline.get("identityFailures"), 0, "offline identity failures")

    identity_fields = ("sessionId", "sessionTag", "wholeFileDigest")
    for field in identity_fields:
        _expect_equal(live.get(field), encoder.get(field), f"live/Encoder {field}")
        _expect_equal(offline.get(field), encoder.get(field), f"offline/Encoder {field}")
    _expect_equal(live.get("originalFileBytes"), encoder.get("fileBytes"), "live original file bytes")
    _expect_equal(offline.get("originalFileBytes"), encoder.get("fileBytes"), "offline original file bytes")

    source_identity = _seal(arguments.source_file)
    live_output_identity = _seal(arguments.live_output)
    offline_output_identity = _seal(arguments.offline_output)
    if source_identity["bytes"] != encoder.get("fileBytes") or live_output_identity["bytes"] != source_identity["bytes"] or \
            offline_output_identity["bytes"] != source_identity["bytes"]:
        _fail("source/live/offline length equality failed")
    if live_output_identity["sha256"] != source_identity["sha256"] or \
            offline_output_identity["sha256"] != source_identity["sha256"]:
        _fail("source/live/offline SHA-256 equality failed")
    _expect_equal(Path(live.get("outputPath", "")).resolve(strict=True), arguments.live_output.resolve(strict=True),
                  "live report output path")
    _expect_equal(Path(offline.get("outputPath", "")).resolve(strict=True), arguments.offline_output.resolve(strict=True),
                  "offline report output path")
    _validate_output_directory(arguments.live_output, "live output")
    _validate_output_directory(arguments.offline_output, "offline output")

    replay_identity = _seal(arguments.replay)
    live_replay = _require_dict(live.get("replay"), "live Replay")
    offline_replay = _require_dict(offline.get("replay"), "offline Replay")
    _expect_equal(live_replay.get("enabled"), True, "live Replay enabled")
    _expect_equal(live_replay.get("offlineMode"), False, "live Replay mode")
    _expect_equal(live_replay.get("evidenceValid"), True, "live Replay evidence")
    _expect_equal(live_replay.get("finalized"), True, "live Replay finalized")
    _expect_equal(live_replay.get("droppedFrames"), 0, "live Replay dropped frames")
    _expect_equal(live_replay.get("fileBytes"), replay_identity["bytes"], "live Replay bytes")
    _expect_equal(Path(live_replay.get("path", "")).resolve(strict=True), arguments.replay.resolve(strict=True),
                  "live Replay path")
    _expect_equal(offline_replay.get("enabled"), True, "offline Replay enabled")
    _expect_equal(offline_replay.get("offlineMode"), True, "offline Replay mode")
    _expect_equal(offline_replay.get("evidenceValid"), True, "offline Replay evidence")
    _expect_equal(offline_replay.get("finalized"), True, "offline Replay finalized")
    _expect_equal(offline_replay.get("fileBytes"), replay_identity["bytes"], "offline Replay bytes")
    _expect_equal(Path(offline_replay.get("path", "")).resolve(strict=True), arguments.replay.resolve(strict=True),
                  "offline Replay path")
    _expect_equal(offline_replay.get("offlineCaptureFrames"), live_replay.get("writtenFrames"),
                  "Replay captured-frame parity")
    _expect_equal(offline_replay.get("offlineDemodResults"), offline_replay.get("offlineCaptureFrames"),
                  "offline production demod coverage")
    _expect_equal(offline_replay.get("offlineObservationMismatches"), 0,
                  "offline observation mismatch")

    encoder_journal_identity, encoder_journal = _validate_journal(
        arguments.encoder_journal, encoder, "Encoder", require_single_monitor_fullscreen=True)
    live_journal_identity, live_journal = _validate_journal(
        arguments.live_decoder_journal, live, "Decoder", require_single_monitor_fullscreen=False)
    offline_journal_identity, offline_journal = _validate_journal(
        arguments.offline_decoder_journal, offline, "Decoder", require_single_monitor_fullscreen=False)
    used_run_id_identity = _validate_used_run_id(arguments.used_run_id, arguments.expected_run_id)
    live_exit_identity = _validate_exit_code(arguments.live_exit_code, "live Decoder exit code")
    offline_exit_identity = _validate_exit_code(arguments.offline_exit_code, "offline Decoder exit code")

    delivery, delivery_identity = _load_json(arguments.delivery_manifest)
    package_verification, package_verification_identity = _load_json(arguments.package_verification)
    _expect_equal(delivery.get("schema"), "PixelBridge.SingleMonitorFullscreenDelivery.4", "delivery schema")
    _expect_equal(delivery.get("sharedRunId"), arguments.expected_run_id, "delivery RunId")
    _expect_equal(delivery.get("gitCommit"), arguments.expected_git_commit, "delivery git commit")
    _expect_equal(delivery.get("runtimeExecutable"), "RuntimePackage/PixelBridgeEncoder.exe",
                  "delivery runtime path")
    presentation = _require_dict(delivery.get("presentation"), "delivery presentation")
    _expect_equal(presentation.get("mode"), "UserAuthorizedSingleMonitorFullscreen", "delivery sender mode")
    _expect_equal(presentation.get("formalDualMonitorGate"), False, "delivery formal dual-monitor Gate")
    _expect_equal(presentation.get("logicalFps"), 2, "delivery logical FPS")
    source_entry = _find_file_entry(delivery, "random-1MiB.bin")
    _expect_equal(source_entry.get("size"), source_identity["bytes"], "delivery source size")
    _expect_equal(source_entry.get("sha256"), source_identity["sha256"], "delivery source SHA-256")
    executable_entry = _find_file_entry(delivery, "RuntimePackage/PixelBridgeEncoder.exe")
    executable_identity = _seal_windows_executable(
        arguments.delivery_manifest.parent / "RuntimePackage" / "PixelBridgeEncoder.exe")
    _expect_equal(executable_entry.get("size"), executable_identity["bytes"], "delivery executable size")
    _expect_equal(executable_entry.get("sha256"), executable_identity["sha256"],
                  "delivery executable SHA-256")
    _expect_equal(package_verification.get("schema"),
                  "PixelBridge.SingleMonitorFullscreenDeliveryVerification.1", "package verification schema")
    _expect_equal(package_verification.get("verified"), True, "package verification result")
    _expect_equal(package_verification.get("sharedRunId"), arguments.expected_run_id,
                  "package verification RunId")
    archive_sha = _require_string(package_verification.get("archiveSha256"), "package archive SHA-256")
    if not SHA256_PATTERN.fullmatch(archive_sha):
        _fail("package archive SHA-256 is invalid")

    receiver_verification, receiver_verification_identity = _load_json(arguments.receiver_verification)
    _expect_equal(receiver_verification.get("schema"),
                  "PixelBridge.SingleMonitorFullscreenStep20ReceiverVerification.1",
                  "receiver verification schema")
    _expect_equal(receiver_verification.get("successfulReceiverChain"), True,
                  "receiver verification result")
    _expect_equal(receiver_verification.get("runId"), arguments.expected_run_id,
                  "receiver verification RunId")
    receiver_truth = _require_dict(receiver_verification.get("truthBoundary"),
                                   "receiver verification truth boundary")
    _expect_equal(receiver_truth.get("formalDualMonitorSenderGate"), False,
                  "receiver verification formal sender Gate")
    _expect_equal(receiver_truth.get("senderMode"), "UserAuthorizedSingleMonitorFullscreen",
                  "receiver verification sender mode")

    combined, combined_identity = _load_json(arguments.combined_report)
    _expect_equal(combined.get("schema"), "PixelBridge.RemoteVisualCombinedReport.1", "combined schema")
    _expect_equal(combined.get("runId"), arguments.expected_run_id, "combined RunId")
    _expect_equal(combined.get("formalMergedRun"), True, "combined formal merge")
    _expect_equal(combined.get("identityStatus"), "Complete", "combined identity")
    _expect_equal(combined.get("evidenceValid"), True, "combined evidence")
    _expect_equal(combined.get("successfulRun"), True, "combined successful run")
    _expect_equal(combined.get("certifiedRemoteVisualProfile"), False, "combined certification")
    combined_artifacts = [arguments.encoder_journal, arguments.used_run_id, arguments.live_decoder_journal,
                          arguments.live_exit_code, arguments.offline_decoder_report,
                          arguments.offline_decoder_journal, arguments.offline_exit_code,
                          arguments.offline_output]
    try:
        recreated_combined = report_tool.merge_reports(
            encoder, live, decoder_clock_offset_ms=arguments.decoder_clock_offset_ms,
            clock_uncertainty_ms=arguments.clock_uncertainty_ms, source_file=arguments.source_file,
            published_file=arguments.live_output, replay_files=[arguments.replay],
            artifact_files=combined_artifacts, encoder_report_file=arguments.encoder_report,
            decoder_report_file=arguments.live_decoder_report)
    except report_tool.ReportError as error:
        raise PilotVerificationError(str(error)) from error
    if recreated_combined != combined:
        _fail("combined report does not equal a fresh strict merge of the sealed inputs")

    encoder_end = _require_integer(encoder.get("runEndedUnixMilliseconds"), "Encoder end", minimum=1)
    live_end = _require_integer(live.get("runEndedUnixMilliseconds"), "live Decoder end", minimum=1)
    raw_post_decoder_delta = encoder_end - live_end
    artifacts = [source_identity, live_output_identity, offline_output_identity, replay_identity,
                 encoder_identity, encoder_journal_identity, used_run_id_identity, live_identity,
                 live_journal_identity, live_exit_identity, offline_identity, offline_journal_identity,
                 offline_exit_identity, delivery_identity, executable_identity, package_verification_identity,
                 receiver_verification_identity, combined_identity]
    return {
        "schema": "PixelBridge.SingleMonitorFullscreenFilePilotVerification.1",
        "createdUtc": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
        "verifiedFileRecoveryChain": True,
        "formalStep20PilotAccepted": False,
        "runId": arguments.expected_run_id,
        "gitCommit": arguments.expected_git_commit,
        "profile": {
            "name": encoder["profile"],
            "visualProfileId": encoder["visualProfileId"],
            "layoutVersion": encoder["visualLayoutVersion"],
            "configuredLogicalVisualFps": encoder["configuredLogicalVisualFps"],
            "generatedVisualFramesPerSecond": generated_fps,
            "configuredLogicalDwellMilliseconds": configured_dwell,
            "minimumObservedLogicalDwellMilliseconds": minimum_dwell,
        },
        "identity": {
            "sessionId": encoder["sessionId"],
            "sessionTag": str(encoder["sessionTag"]),
            "wholeFileDigest": encoder["wholeFileDigest"],
            "sourceBytes": source_identity["bytes"],
            "sourceSha256": source_identity["sha256"],
            "liveSha256": live_output_identity["sha256"],
            "offlineSha256": offline_output_identity["sha256"],
        },
        "sender": {
            "state": encoder["state"],
            "sourceStable": encoder["sourceStable"],
            "manualStopWithoutCompletionInference": True,
            "surface": data_window,
            "journalSamples": encoder_journal["samples"],
            "logicalDwellViolationCount": encoder["logicalDwellViolationCount"],
        },
        "liveReceiver": {
            "state": live["state"],
            "actualBackend": live["actualBackend"],
            "wholeFileDigestVerified": live["wholeFileDigestVerified"],
            "finalPublishSucceeded": live["finalPublishSucceeded"],
            "recoveryRuntimeMilliseconds": live["recoveryRuntimeMilliseconds"],
            "uniqueVisualFps": live["uniqueVisualFps"],
            "endToEndUniqueVisualFps": live["endToEndUniqueVisualFps"],
            "acceptedTransportBlocks": live["acceptedTransportBlocks"],
            "outerUniqueSymbols": live_outer["uniqueSymbols"],
            "fecFailures": live["fecFailures"],
            "crcFailures": live["crcFailures"],
            "identityFailures": live["identityFailures"],
            "resourceRejections": live_outer["resourceRejections"],
            "conflictRejections": live_outer["conflictRejections"],
            "journalSamples": live_journal["samples"],
        },
        "replay": {
            "bytes": replay_identity["bytes"],
            "sha256": replay_identity["sha256"],
            "sampleFps": live_replay["maximumCaptureFramesPerSecond"],
            "writtenFrames": live_replay["writtenFrames"],
            "droppedFrames": live_replay["droppedFrames"],
            "finalized": live_replay["finalized"],
            "evidenceValid": live_replay["evidenceValid"],
        },
        "offlineReceiver": {
            "state": offline["state"],
            "wholeFileDigestVerified": offline["wholeFileDigestVerified"],
            "finalPublishSucceeded": offline["finalPublishSucceeded"],
            "recoveryRuntimeMilliseconds": offline["recoveryRuntimeMilliseconds"],
            "captureFrames": offline_replay["offlineCaptureFrames"],
            "demodResults": offline_replay["offlineDemodResults"],
            "observationMismatches": offline_replay["offlineObservationMismatches"],
            "acceptedTransportBlocks": offline["acceptedTransportBlocks"],
            "outerUniqueSymbols": offline_outer["uniqueSymbols"],
            "fecFailures": offline["fecFailures"],
            "crcFailures": offline["crcFailures"],
            "identityFailures": offline["identityFailures"],
            "resourceRejections": offline_outer["resourceRejections"],
            "conflictRejections": offline_outer["conflictRejections"],
            "journalSamples": offline_journal["samples"],
        },
        "strictCombinedReport": {
            "identity": combined_identity,
            "formalMergedRun": combined["formalMergedRun"],
            "identityStatus": combined["identityStatus"],
            "evidenceValid": combined["evidenceValid"],
            "successfulRun": combined["successfulRun"],
            "certifiedRemoteVisualProfile": combined["certifiedRemoteVisualProfile"],
        },
        "rawCrossComputerTiming": {
            "encoderStartedUnixMilliseconds": encoder["runStartedUnixMilliseconds"],
            "encoderEndedUnixMilliseconds": encoder_end,
            "decoderStartedUnixMilliseconds": live["runStartedUnixMilliseconds"],
            "decoderEndedUnixMilliseconds": live_end,
            "rawEncoderEndMinusDecoderEndMilliseconds": raw_post_decoder_delta,
            "decoderClockOffsetMillisecondsUsedByCombinedReport": arguments.decoder_clock_offset_ms,
            "clockUncertaintyMillisecondsUsedByCombinedReport": arguments.clock_uncertainty_ms,
            "independentClockCalibrationMeasured": arguments.clock_calibration_measured,
            "formalPostDecoderBroadcastProofClaimed": arguments.clock_calibration_measured and raw_post_decoder_delta > 0,
        },
        "truthBoundary": {
            "actualRemotePixelChain": True,
            "decoderUsedOnlyCapturedPixels": True,
            "senderMode": "UserAuthorizedSingleMonitorFullscreen",
            "formalDualMonitorSenderGate": False,
            "formalPilotPlanAndEndpointProcessChain": False,
            "providerUiProvenance": "NotProvided",
            "independentCrossComputerClockCalibration": arguments.clock_calibration_measured,
            "outerPictureAspectRatioRequired": False,
            "logicalCanvas": "centered exact 1920x1080",
            "actualEstimatedScaleX": live["remoteMetadata"]["estimatedScaleX"],
            "actualEstimatedScaleY": live["remoteMetadata"]["estimatedScaleY"],
            "arbitraryGeometryCertified": False,
            "falseAcceptedCodewords": None,
            "falseAcceptedCodewordsReason": live["falseAcceptedCodewordsUnavailableReason"],
            "zeroFalsePublishedOutput": True,
            "certifiedRemoteVisualProfile": False,
        },
        "artifacts": artifacts,
    }


def _path_argument(value: str) -> Path:
    return Path(value)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder-report", required=True, type=_path_argument)
    parser.add_argument("--encoder-journal", required=True, type=_path_argument)
    parser.add_argument("--used-run-id", required=True, type=_path_argument)
    parser.add_argument("--live-decoder-report", required=True, type=_path_argument)
    parser.add_argument("--live-decoder-journal", required=True, type=_path_argument)
    parser.add_argument("--live-exit-code", required=True, type=_path_argument)
    parser.add_argument("--offline-decoder-report", required=True, type=_path_argument)
    parser.add_argument("--offline-decoder-journal", required=True, type=_path_argument)
    parser.add_argument("--offline-exit-code", required=True, type=_path_argument)
    parser.add_argument("--source-file", required=True, type=_path_argument)
    parser.add_argument("--live-output", required=True, type=_path_argument)
    parser.add_argument("--offline-output", required=True, type=_path_argument)
    parser.add_argument("--replay", required=True, type=_path_argument)
    parser.add_argument("--combined-report", required=True, type=_path_argument)
    parser.add_argument("--receiver-verification", required=True, type=_path_argument)
    parser.add_argument("--delivery-manifest", required=True, type=_path_argument)
    parser.add_argument("--package-verification", required=True, type=_path_argument)
    parser.add_argument("--expected-run-id", required=True)
    parser.add_argument("--expected-git-commit", required=True)
    parser.add_argument("--decoder-clock-offset-ms", type=int, default=0)
    parser.add_argument("--clock-uncertainty-ms", type=int, default=5_000)
    parser.add_argument("--clock-calibration-measured", action="store_true")
    parser.add_argument("--output", required=True, type=_path_argument)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        verification = _verify(arguments)
        data = (json.dumps(verification, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
        report_tool._write_new(arguments.output, data)
        identity = _seal(arguments.output)
        print(json.dumps({"verification": identity, "verifiedFileRecoveryChain": True,
                          "formalStep20PilotAccepted": False}, separators=(",", ":")))
        return 0
    except (OSError, PilotVerificationError, report_tool.ReportError) as error:
        print(f"SingleMonitorFilePilotVerification: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
