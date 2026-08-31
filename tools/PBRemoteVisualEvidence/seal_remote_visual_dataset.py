#!/usr/bin/env python3
"""Seal bounded RemoteVisual screenshots and Replay v2 files into a canonical index.

The input manifest names files relative to an explicit artifact root. The tool
never captures a display, crops or rewrites evidence, follows a symlink, or
turns diagnostic geometry into decoder acceptance. Screenshot pixels are read
only by the existing bounded analyzer; Replay v2 files are indexed as sealed
artifacts and remain subject to ReplayV2Reader/offline-decoder validation.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable, Iterable
import hashlib
import json
import os
from pathlib import Path
import stat as stat_module
import sys
from typing import Any, BinaryIO

import blake3

from analyze_remote_capture import analyze


INPUT_SCHEMA = "PixelBridge.RemoteVisualDatasetInput.1"
OUTPUT_SCHEMA = "PixelBridge.RemoteVisualDatasetIndex.1"
MAX_MANIFEST_BYTES = 256 * 1024
MAX_ARTIFACTS = 64
MAX_REPLAY_BYTES = 16 * 1024 * 1024 * 1024
HASH_CHUNK_BYTES = 1024 * 1024
VALID_KINDS = frozenset({"Screenshot", "ReplayV2"})
VALID_PROFILES = frozenset({"Direct-Level", "Shape+Chroma", "PB-RemoteVisual-LF4-X1", "Unknown"})
VALID_PROVENANCE = frozenset({"RealRemoteRender", "ActualCodecOffline", "DeterministicSimulator", "SyntheticTest"})
VALID_CAPTURE_SCOPES = frozenset({"ExperimentMonitorOnly", "SelectedRoiOnly"})


class DatasetError(RuntimeError):
    pass


def reject_duplicate_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DatasetError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def reject_non_finite(value: str) -> None:
    raise DatasetError(f"non-finite JSON number: {value}")


def parse_manifest(path: Path) -> tuple[dict[str, Any], bytes]:
    if path.is_symlink():
        raise DatasetError("input manifest is not a bounded regular file")
    try:
        with path.open("rb") as stream:
            stat_before = os.fstat(stream.fileno())
            if not stat_module.S_ISREG(stat_before.st_mode) or stat_before.st_size <= 0 or \
                    stat_before.st_size > MAX_MANIFEST_BYTES:
                raise DatasetError("input manifest is not a bounded regular file")
            contents = stream.read(MAX_MANIFEST_BYTES + 1)
            stat_after = os.fstat(stream.fileno())
        path_after = path.stat()
    except OSError as exception:
        raise DatasetError(f"cannot read input manifest: {path}") from exception
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    if len(contents) != stat_before.st_size or len(contents) > MAX_MANIFEST_BYTES or \
            any(getattr(stat_before, field) != getattr(stat_after, field) for field in stable_fields) or \
            any(getattr(stat_after, field) != getattr(path_after, field) for field in stable_fields):
        raise DatasetError("input manifest changed while being read")
    try:
        value = json.loads(contents, object_pairs_hook=reject_duplicate_members,
                           parse_constant=reject_non_finite)
    except (UnicodeDecodeError, json.JSONDecodeError) as exception:
        raise DatasetError(f"input manifest is not valid UTF-8 JSON: {exception}") from exception
    if not isinstance(value, dict):
        raise DatasetError("input manifest root must be an object")
    return value, contents


def require_exact_keys(value: dict[str, Any], required: set[str], optional: set[str], label: str) -> None:
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    if missing:
        raise DatasetError(f"{label} is missing members: {sorted(missing)}")
    if unknown:
        raise DatasetError(f"{label} has unknown members: {sorted(unknown)}")


def require_string(value: Any, label: str, maximum_length: int) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum_length or "\x00" in value:
        raise DatasetError(f"{label} must be a nonempty bounded string")
    return value


def resolve_artifact(root: Path, relative_value: Any) -> tuple[Path, str]:
    relative = Path(require_string(relative_value, "artifact path", 1024))
    if relative.is_absolute():
        raise DatasetError("artifact path must be relative to --artifact-root")
    if any(":" in component for component in relative.parts):
        raise DatasetError("artifact path must not name a drive-relative path or alternate data stream")
    root = root.resolve(strict=True)
    candidate = root
    for component in relative.parts:
        candidate /= component
        is_junction = getattr(candidate, "is_junction", lambda: False)
        if candidate.is_symlink() or is_junction():
            raise DatasetError(f"artifact path contains a symlink or junction: {relative}")
    try:
        resolved = (root / relative).resolve(strict=True)
        normalized = resolved.relative_to(root).as_posix()
    except (OSError, ValueError) as exception:
        raise DatasetError(f"artifact path escapes or does not exist under the artifact root: {relative}") from exception
    if resolved.is_symlink() or not resolved.is_file():
        raise DatasetError(f"artifact is not a regular non-symlink file: {relative}")
    return resolved, normalized


def hash_open_stream(stream: BinaryIO, maximum_bytes: int) -> tuple[int, str, str]:
    sha256 = hashlib.sha256()
    blake3_digest = blake3.blake3()
    total = 0
    while True:
        chunk = stream.read(HASH_CHUNK_BYTES)
        if not chunk:
            break
        total += len(chunk)
        if total > maximum_bytes:
            raise DatasetError(f"artifact exceeds the {maximum_bytes}-byte resource limit")
        sha256.update(chunk)
        blake3_digest.update(chunk)
    return total, sha256.hexdigest(), blake3_digest.hexdigest()


def hash_stable_file(path: Path, maximum_bytes: int,
                     validator: Callable[[BinaryIO, int], None] | None = None) -> tuple[int, str, str]:
    if path.is_symlink():
        raise DatasetError(f"artifact became a symlink: {path}")
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat_module.S_ISREG(before.st_mode) or before.st_size <= 0 or before.st_size > maximum_bytes:
            raise DatasetError(f"artifact has an invalid byte count: {path}")
        if validator is not None:
            validator(stream, before.st_size)
            stream.seek(0)
        size, sha256, blake3_hex = hash_open_stream(stream, maximum_bytes)
        after = os.fstat(stream.fileno())
    path_after = path.stat()
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    if size != before.st_size or any(getattr(before, field) != getattr(after, field) for field in stable_fields) or \
            any(getattr(after, field) != getattr(path_after, field) for field in stable_fields):
        raise DatasetError(f"artifact changed while being hashed: {path}")
    return size, sha256, blake3_hex


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def validate_replay_envelope(stream: BinaryIO, size: int) -> None:
    if size < 352:
        raise DatasetError("ReplayV2 file is too small for its fixed header and footer")
    header = stream.read(12)
    stream.seek(-96, os.SEEK_END)
    footer = stream.read(12)
    if header[:8] != b"PBRCV002" or int.from_bytes(header[8:10], "little") != 2 or \
            int.from_bytes(header[10:12], "little") != 256:
        raise DatasetError("ReplayV2 file header envelope is invalid")
    if footer[:8] != b"PBRVF002" or int.from_bytes(footer[8:10], "little") != 2 or \
            int.from_bytes(footer[10:12], "little") != 96:
        raise DatasetError("ReplayV2 file footer envelope is invalid")


def validate_input(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    require_exact_keys(manifest, {"schema", "datasetId", "artifacts"}, {"notes"}, "manifest")
    if manifest["schema"] != INPUT_SCHEMA:
        raise DatasetError(f"unsupported input schema: {manifest['schema']}")
    dataset_id = require_string(manifest["datasetId"], "datasetId", 32)
    if len(dataset_id) != 32 or any(character not in "0123456789abcdef" for character in dataset_id):
        raise DatasetError("datasetId must be 128-bit lowercase hexadecimal")
    if "notes" in manifest:
        require_string(manifest["notes"], "notes", 4096)
    artifacts = manifest["artifacts"]
    if not isinstance(artifacts, list) or not 1 <= len(artifacts) <= MAX_ARTIFACTS:
        raise DatasetError(f"artifacts must contain 1..{MAX_ARTIFACTS} entries")
    for index, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict):
            raise DatasetError(f"artifact {index} must be an object")
        require_exact_keys(artifact,
                           {"path", "kind", "profile", "provenance", "captureScope",
                            "containsProtectedMonitorPixels"},
                           {"notes"}, f"artifact {index}")
        kind = require_string(artifact["kind"], f"artifact {index} kind", 32)
        profile = require_string(artifact["profile"], f"artifact {index} profile", 64)
        provenance = require_string(artifact["provenance"], f"artifact {index} provenance", 64)
        capture_scope = require_string(artifact["captureScope"], f"artifact {index} captureScope", 64)
        if kind not in VALID_KINDS or profile not in VALID_PROFILES or provenance not in VALID_PROVENANCE or \
                capture_scope not in VALID_CAPTURE_SCOPES:
            raise DatasetError(f"artifact {index} contains an unsupported enum value")
        if not isinstance(artifact["containsProtectedMonitorPixels"], bool):
            raise DatasetError(f"artifact {index} containsProtectedMonitorPixels must be boolean")
        if artifact["containsProtectedMonitorPixels"]:
            raise DatasetError(f"artifact {index} contains protected-monitor pixels")
        if kind == "ReplayV2" and capture_scope != "SelectedRoiOnly":
            raise DatasetError(f"artifact {index} ReplayV2 must be SelectedRoiOnly")
        if "notes" in artifact:
            require_string(artifact["notes"], f"artifact {index} notes", 4096)
    return artifacts


def seal_dataset(manifest_path: Path, artifact_root: Path) -> dict[str, Any]:
    manifest, manifest_bytes = parse_manifest(manifest_path)
    artifacts = validate_input(manifest)
    sealed_artifacts: list[dict[str, Any]] = []
    normalized_artifact_paths: set[str] = set()
    for artifact_input in artifacts:
        path, normalized_path = resolve_artifact(artifact_root, artifact_input["path"])
        normalized_key = os.path.normcase(normalized_path)
        if normalized_key in normalized_artifact_paths:
            raise DatasetError(f"duplicate normalized artifact path: {normalized_path}")
        normalized_artifact_paths.add(normalized_key)
        kind = artifact_input["kind"]
        maximum_bytes = MAX_REPLAY_BYTES if kind == "ReplayV2" else MAX_MANIFEST_BYTES * 256
        validator = validate_replay_envelope if kind == "ReplayV2" else None
        size, sha256, blake3_hex = hash_stable_file(path, maximum_bytes, validator)
        sealed: dict[str, Any] = {
            "path": normalized_path,
            "kind": kind,
            "profile": artifact_input["profile"],
            "provenance": artifact_input["provenance"],
            "captureScope": artifact_input["captureScope"],
            "containsProtectedMonitorPixels": False,
            "bytes": size,
            "sha256": sha256,
            "blake3": blake3_hex,
        }
        if "notes" in artifact_input:
            sealed["notes"] = artifact_input["notes"]
        if kind == "Screenshot":
            analysis = analyze(path)
            if analysis["sourceBytes"] != size or analysis["sha256"].lower() != sha256:
                raise DatasetError(f"screenshot analyzer identity mismatch: {normalized_path}")
            sealed["diagnosticAnalysis"] = analysis
            sealed["semanticValidation"] = "BoundedScreenshotAnalyzer"
        else:
            if size < 256 or path.suffix.lower() != ".pbrv2":
                raise DatasetError(f"ReplayV2 artifact has an invalid extension or size: {normalized_path}")
            sealed["envelopeValidation"] = "ReplayV2HeaderFooterVersionAndSize"
            sealed["semanticValidation"] = "RequiredViaReplayV2ReaderBeforeEvidenceUse"
        sealed_artifacts.append(sealed)

    payload = {
        "datasetId": manifest["datasetId"],
        "inputManifest": {
            "bytes": len(manifest_bytes),
            "sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            "blake3": blake3.blake3(manifest_bytes).hexdigest(),
        },
        "artifacts": sealed_artifacts,
        "summary": {
            "artifactCount": len(sealed_artifacts),
            "screenshotCount": sum(artifact["kind"] == "Screenshot" for artifact in sealed_artifacts),
            "replayV2Count": sum(artifact["kind"] == "ReplayV2" for artifact in sealed_artifacts),
            "realRemoteRenderCount": sum(artifact["provenance"] == "RealRemoteRender"
                                         for artifact in sealed_artifacts),
            "profiles": sorted({artifact["profile"] for artifact in sealed_artifacts}),
            "containsProtectedMonitorPixels": False,
            "acceptanceInput": False,
            "providerSpecificThresholds": False,
        },
    }
    if "notes" in manifest:
        payload["notes"] = manifest["notes"]
    payload_bytes = canonical_json(payload).rstrip(b"\n")
    return {
        "schema": OUTPUT_SCHEMA,
        "version": 1,
        "payloadBlake3": blake3.blake3(payload_bytes).hexdigest(),
        "payload": payload,
    }


def publish_new(path: Path, contents: bytes) -> None:
    if path.exists():
        raise DatasetError(f"output already exists: {path}")
    partial = Path(f"{path}.partial")
    if partial.exists():
        raise DatasetError(f"partial output already exists: {partial}")
    try:
        with partial.open("xb") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        partial.rename(path)
    except BaseException:
        try:
            partial.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def parse_arguments(arguments: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-manifest", required=True, type=Path)
    parser.add_argument("--artifact-root", required=True, type=Path)
    parser.add_argument("--output-index", required=True, type=Path)
    return parser.parse_args(arguments)


def main(arguments: list[str]) -> int:
    options = parse_arguments(arguments)
    try:
        index = seal_dataset(options.input_manifest, options.artifact_root)
        publish_new(options.output_index, canonical_json(index))
    except (DatasetError, OSError, ValueError) as exception:
        print(f"[error] {exception}", file=sys.stderr)
        return 1
    print(json.dumps({
        "schema": index["schema"],
        "outputIndex": str(options.output_index.resolve()),
        **index["payload"]["summary"],
    }, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
