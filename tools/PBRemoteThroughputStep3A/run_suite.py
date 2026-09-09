"""Bounded, create-only Step3-A orchestration. No screen, network or input automation."""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import time

from blake3 import blake3

CASES = ("clean", "repeat-each", "burst-loss", "neutral-chroma", "quantize-6",
         "bootstrap-conflict", "local-freshness-mix", "marker-plus-one", "shift-right-one")
FRAME_BYTES = 1920 * 1080 * 4
JSON_LIMIT = 1024 * 1024
TREE_LIMIT = 128 * 1024 * 1024
TREE_ENTRY_LIMIT = 2048
SOURCE_NAME = "step3a-source.bin"
DOMAIN = b"PixelBridge.RemoteVisualChannelImage.1\0" + struct.pack("<II", 1920, 1080)
BASELINE_FINGERPRINT = "9cfb4e45625564e2ea70b2241779ff01cf89aedc095163f098c91c2784bd86ff"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def local_path(value):
    path = Path(value)
    require(path.is_absolute() and len(path.drive) == 2, "Explicit absolute local path required")
    require(ctypes.windll.kernel32.GetDriveTypeW(str(path.anchor)) == 3, "Fixed local drive required")
    for component in (*reversed(path.parents), path):
        if component.exists():
            require(not component.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT,
                    "Reparse paths are not accepted")
    return path.resolve()


def bounded_bytes(path, limit):
    require(path.is_file() and path.stat().st_size <= limit, f"Missing or oversized input: {path}")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, "Input grew beyond resource limit")
    return data


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(data):
    def reject_constant(value):
        raise ValueError(f"Non-finite JSON constant: {value}")
    return json.loads(data, object_pairs_hook=unique_object, parse_constant=reject_constant)


def read_json(path):
    return parse_json(bounded_bytes(path, JSON_LIMIT).decode("utf-8"))


def read_rows(path):
    lines = bounded_bytes(path, JSON_LIMIT).splitlines()
    require(0 < len(lines) <= 8, "Trace observation count outside fixed budget")
    require(all(0 < len(line) <= 65536 for line in lines), "Trace line limit exceeded")
    return [parse_json(line.decode("utf-8")) for line in lines]


def write_json(path, value):
    data = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    require(len(data) <= JSON_LIMIT, "Analysis JSON size limit exceeded")
    with path.open("xb") as stream:
        stream.write(data)


def tree_identity(root):
    rows = []
    total = 0
    entries = 0

    def visit(parent):
        nonlocal total, entries
        require(len(parent.relative_to(root).parts) <= 16, "Artifact depth limit exceeded")
        # scandir is consumed incrementally; empty directory forests must not
        # escape the file-count bound, nor allocate an unbounded os.walk list.
        with os.scandir(parent) as iterator:
            for entry in iterator:
                entries += 1
                require(entries <= TREE_ENTRY_LIMIT, "Artifact entry limit exceeded")
                item = Path(entry.path)
                attributes = entry.stat(follow_symlinks=False)
                require(not attributes.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT, "Artifact reparse point rejected")
                if entry.is_dir(follow_symlinks=False):
                    visit(item)
                    continue
                require(entry.is_file(follow_symlinks=False), "Non-regular artifact rejected")
                size = attributes.st_size
                total += size
                require(len(rows) < 1024 and total <= TREE_LIMIT, "Artifact tree limit exceeded")
                digest = hashlib.sha256()
                consumed = 0
                with item.open("rb") as stream:
                    while block := stream.read(JSON_LIMIT):
                        consumed += len(block)
                        require(consumed <= size, "Artifact grew during hashing")
                        digest.update(block)
                require(consumed == size and item.stat().st_size == size, "Artifact changed during hashing")
                rows.append({"path": item.relative_to(root).as_posix(), "bytes": size, "sha256": digest.hexdigest()})

    visit(root)
    return {"bytes": total, "files": sorted(rows, key=lambda row: row["path"])}


def load_fixture(root):
    tree_identity(root)
    manifest = read_json(root / "fixture.json")
    require(manifest["schema"] == "PixelBridge.Step3A.FrozenPixels.1", "Wrong fixture schema")
    require(manifest["producer"] == "ProductionEncoderRuntime" and
            manifest["sessionIdentity"] == "OS-CSPRNG-frozen-in-pixels", "Wrong fixture producer contract")
    require((manifest["width"], manifest["height"], manifest["rowPitch"], manifest["pixelFormat"],
             manifest["frameCount"], manifest["sourceBytes"], manifest["codec"]) ==
            (1920, 1080, 7680, "BGRA8", 4, 512, None), "Fixture raster/count/codec contract changed")
    require(manifest["colorContract"] == "CanonicalSDR_RGB_full_no_conversion", "Color contract changed")
    require(len(manifest["frames"]) == 4, "Exactly four frozen frames required")
    source = bounded_bytes(root / SOURCE_NAME, 512)
    require(source == bytes((index * 73 + 19) & 255 for index in range(512)), "Fixed source bytes changed")
    require(blake3(source).hexdigest() == manifest["sourceBlake3"], "Source digest mismatch")
    frames = []
    for index, record in enumerate(manifest["frames"]):
        require(record["index"] == index and record["file"] == f"frame-{index}.bgra", "Non-canonical frame identity/path")
        pixels = bounded_bytes(root / record["file"], FRAME_BYTES)
        require(len(pixels) == FRAME_BYTES and blake3(pixels).hexdigest() == record["blake3"], "Frozen frame digest/size mismatch")
        marker = pixels[(47 * 1920 + 20) * 4:(47 * 1920 + 20) * 4 + 4]
        frames.append({**record, "domainBlake3": blake3(DOMAIN + pixels).hexdigest(),
                       "markerPlusOneBgra": [min(255, value + 1) if channel < 3 else value for channel, value in enumerate(marker)]})
    require(len({frame["blake3"] for frame in frames}) == 4, "Fixture must contain four distinct production frames")
    return {"manifest": manifest, "frames": frames, "source": source, "identity": tree_identity(root)}


def schedule(name):
    require(name in CASES, "Unknown fixed scenario")
    ordinals = [0, 0, 1, 1, 2, 2, 3, 3] if name == "repeat-each" else [0, 3] if name == "burst-loss" else [0, 1, 2, 3]
    return [{"sourceOrdinal": ordinal, "pts": index if name == "repeat-each" else ordinal * 2,
             "duration": 1 if name == "repeat-each" else 2} for index, ordinal in enumerate(ordinals)]


def invoke(executable, arguments, log_path, timeout=120):
    started = time.monotonic()
    with log_path.open("xb") as log:
        completed = subprocess.run([str(executable), *map(str, arguments)], stdout=log, stderr=subprocess.STDOUT,
                                   timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
    bounded_bytes(log_path, 65536)
    record = {"argv": [str(executable), *map(str, arguments)], "returnCode": completed.returncode,
              "processingSeconds": time.monotonic() - started, "timeoutSeconds": timeout, "createNoWindow": True}
    write_json(log_path.with_suffix(".process.json"), record)
    return completed.returncode


def run_suite(executable, fixture_root, output):
    fixture = load_fixture(fixture_root)
    require(not output.exists(), "Suite output root must be new")
    require(not output.is_relative_to(fixture_root) and not fixture_root.is_relative_to(output), "Suite must be disjoint from frozen input")
    executable_digest = hashlib.sha256(bounded_bytes(executable, 64 * JSON_LIMIT)).hexdigest()
    output.mkdir()
    started = time.monotonic()
    write_json(output / "INPUT_IDENTITY.json", {"fixture": str(fixture_root), "fixtureIdentity": fixture["identity"],
        "executable": str(executable), "executableSha256": executable_digest, "sealedStep2SourceFingerprint": BASELINE_FINGERPRINT,
        "scenarios": list(CASES), "trials": 2, "observations": 76, "totalProcessingBudgetSeconds": 600,
        "scenarioProcessingTimeoutSeconds": 120, "classification": "SyntheticOfflineDiagnostic", "fieldStatus": "NOT_RUN"})
    for trial in (1, 2):
        directory = output / f"trial-{trial:02}"
        directory.mkdir()
        for name in CASES:
            remaining = 600 - (time.monotonic() - started)
            require(remaining > 0, "Suite processing budget expired; do not auto-increase or resume")
            code = invoke(executable, ["--run-case", fixture_root, name, directory / name],
                          directory / f"{name}.log", min(120, remaining))
            require(code == 0, f"Replay execution failed: trial {trial}, {name}; preserve all evidence")
            summary = read_json(directory / name / "summary.json")
            require(not summary["error"] and summary["reachedEof"], "Replay stopped before EOF")
            tree_identity(output)
            print(f"trial={trial} case={name} frames={summary['frames']} published={summary['publishedAndReopened']}", flush=True)
    require(tree_identity(fixture_root) == fixture["identity"], "Frozen input changed during suite")
    require(hashlib.sha256(bounded_bytes(executable, 64 * JSON_LIMIT)).hexdigest() == executable_digest, "Executable changed during suite")
    write_json(output / "RUN_STATUS.json", {"status": "EXECUTED_NOT_YET_ANALYZED", "processingSeconds": time.monotonic() - started,
        "observations": 76, "fieldStatus": "NOT_RUN", "inputUnchanged": True})


def histogram(values):
    return dict(sorted(Counter(str(value) for value in values).items()))


def validate_transform_manifest(name, manifest, frame):
    definitions = {
        "neutral-chroma": (2, "neutral-chroma", {"alpha": "preserved", "matrix": "bt709-integer"}),
        "quantize-6": (2, "channel-quantization", {"bitsPerChannel": 6, "channels": "bgr", "method": "uniform-round-nearest"}),
        "bootstrap-conflict": (1, "block-replacement", {"x": 1216, "y": 1000, "width": 608, "height": 64}),
        "local-freshness-mix": (1, "block-replacement", {"x": 96, "y": 160, "width": 128, "height": 128}),
        "marker-plus-one": (2, "solid-overlay", {"x": 20, "y": 47, "width": 1, "height": 1, "bgra": frame["markerPlusOneBgra"], "opacity": 255}),
        "shift-right-one": (1, "resample", {"filter": "bilinear", "outputWidth": 1920, "outputHeight": 1080,
            "scaleXBinary64": "3ff0000000000000", "scaleYBinary64": "3ff0000000000000", "originXBinary64": "3ff0000000000000",
            "originYBinary64": "0000000000000000", "borderBgra": [128, 128, 128, 255]})}
    expected = definitions.get(name)
    version = expected[0] if expected else 1
    require(manifest["version"] == version and manifest["schema"] == f"PixelBridge.RemoteVisualChannelManifest.{version}", "Transform schema/version mismatch")
    for key in ("source", "output"):
        require((manifest[key]["width"], manifest[key]["height"], manifest[key]["format"]) == (1920, 1080, "bgra8"), "Transform dimensions/format changed")
    require(len(manifest["transforms"]) == (1 if expected else 0), "Unexpected transform chain")
    if expected:
        operation = manifest["transforms"][0]
        require(operation["index"] == 0 and operation["kind"] == expected[1] and operation["parameters"] == expected[2], "Fixed scenario transform parameters changed")
        require(operation["input"] == {key: value for key, value in manifest["source"].items() if key != "format"} and
                operation["output"] == {key: value for key, value in manifest["output"].items() if key != "format"} and
                operation["referenceInputBlake3"] == manifest["referenceBlake3"], "Transform chain linkage mismatch")


def case_analysis(directory, name, fixture, truth):
    summary = read_json(directory / "summary.json")
    rows = read_rows(directory / "frames.jsonl")
    transforms = read_rows(directory / "transforms.jsonl")
    expected = schedule(name)
    require(summary["schema"] == "PixelBridge.Step3A.Run.1" and summary["scenario"] == name, "Wrong case identity")
    require(summary["classification"] == "SyntheticOfflineDiagnostic" and summary["fieldStatus"] == "NOT_RUN" and
            summary["processingDevice"] == "D3D11_WARP" and summary["inputContract"] == "OfflinePixels", "Offline authority changed")
    for key in ("originalCaptureBackend", "originalCursorState", "originalCaptureClock", "codec", "bitrate", "liveChannelGoodput", "simulatedVerifiedGoodput"):
        require(summary[key] is None, f"Unjustified channel/capture claim: {key}")
    require(not summary["error"] and summary["reachedEof"] and not summary["prefixLimitReached"], "Incomplete replay")
    require(len(rows) == len(transforms) == summary["frames"] == summary["expectedObservations"] == len(expected), "Observation count mismatch")
    require(summary["syntheticPtsSpanSeconds"] == (expected[-1]["pts"] - expected[0]["pts"]) / 30, "Synthetic PTS span mismatch")
    normalized_transforms = []
    normalized_rows = []
    for index, (row, transform, point) in enumerate(zip(rows, transforms, expected, strict=True)):
        require(row["observation"] == transform["observation"] == index + 1, "Observation identity mismatch")
        require(row["pts"] == transform["syntheticPts"] == point["pts"] and
                row["duration"] == transform["duration"] == point["duration"], "PTS/duration mismatch")
        require((row["timeBaseNumerator"], row["timeBaseDenominator"], transform["timeBaseNumerator"], transform["timeBaseDenominator"]) == (1, 30, 1, 30), "Timebase mismatch")
        ordinal = point["sourceOrdinal"]
        require(transform["sourceOrdinal"] == ordinal, "Source ordinal mismatch")
        frame = fixture["frames"][ordinal]
        require(transform["sourceRawBlake3"] == frame["blake3"], "Runtime read differs from frozen pixels")
        manifest = transform["transformManifest"]
        validate_transform_manifest(name, manifest, frame)
        require(manifest["source"]["blake3"] == frame["domainBlake3"] and manifest["seedHex"] == "0000535445503341", "Simulator input/seed mismatch")
        encoded_manifest = json.dumps(manifest, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
        require(blake3(encoded_manifest).hexdigest() == transform["canonicalTransformManifestBlake3"], "Canonical transform manifest digest mismatch")
        reference = (ordinal + 1) % 4 if name in ("bootstrap-conflict", "local-freshness-mix") else None
        require(transform["referenceOrdinal"] == reference and manifest["referenceBlake3"] ==
                (fixture["frames"][reference]["domainBlake3"] if reference is not None else None), "Reference pixel provenance mismatch")
        require(len(manifest["transforms"]) == (0 if name in CASES[:3] else 1), "Unexpected transform chain")
        if name in CASES[:3]:
            require(transform["outputRawBlake3"] == frame["blake3"] and manifest["output"]["blake3"] == frame["domainBlake3"], "Identity transform changed pixels")
        else:
            require(transform["outputRawBlake3"] != frame["blake3"], "Distortion had no pixel effect")
        require(len(row["slots"]) == 15 and all(len(slot) == 6 for slot in row["slots"]) and
                len(row["freshnessCurrent"]) == 9 and len(row["bootstrapCopies"]) == 2, "Diagnostic shape changed")
        require(len(row["acceptedPayloadDigests"]) == row["acceptedBlocks"] == sum(slot[5] for slot in row["slots"]), "Accepted block accounting mismatch")
        require(len({record[0] for record in row["acceptedPayloadDigests"]}) == row["acceptedBlocks"], "Repeated accepted codeword slot")
        if truth is not None:
            reference_row = truth[ordinal]
            if row["acceptedBlocks"]:
                require((row["sessionTag"], row["frameSequence"]) == (reference_row["sessionTag"], reference_row["frameSequence"]), "Accepted wrong frame identity")
            expected_payloads = {record[0]: record for record in reference_row["acceptedPayloadDigests"]}
            require(all(record == expected_payloads.get(record[0]) for record in row["acceptedPayloadDigests"]), "Accepted payload differs from same-ordinal clean decode")
        normalized_rows.append({key: value for key, value in row.items() if key != "processingQpc100ns"})
        normalized_transforms.append({key: value for key, value in transform.items() if key != "transformProcessingNanoseconds"})
    report = summary["receiverReport"]
    publish = report["publish"]
    flags = {key: publish[key] for key in ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published")}
    published = summary["publishedAndReopened"]
    require(published == flags["published"], "Publication surface disagreement")
    final_file = directory / "output" / SOURCE_NAME
    independent = None
    if published:
        require(all(flags.values()) and Path(publish["finalPath"]).resolve() == final_file.resolve(), "Final path or verification gate mismatch")
        recovered = bounded_bytes(final_file, 512)
        require(recovered == fixture["source"] and blake3(recovered).hexdigest() == publish["wholeFileDigest"], "Independent final file verification failed")
        require(report["recovery"]["verifiedRawBytes"] == 512, "Verified raw-byte mismatch")
        independent = {"bytes": len(recovered), "sha256": hashlib.sha256(recovered).hexdigest(), "blake3": blake3(recovered).hexdigest(), "equalToFrozenSource": True}
    else:
        require(not final_file.exists() and not any(flags.values()), "Rejected case published or partially claimed successful verification")
    require(report["unifiedTelemetry"]["uniqueVisualFps"] is None and
            report["recovery"]["currentVerifiedRawGoodputBytesPerSecond"] is None, "Offline diagnostic acquired live rate authority")
    slots = [slot for row in rows for slot in row["slots"]]
    reached = [row for row in rows if row["kind"] == 4 and row["geometry"] == 1 and row["frameErasure"] == 0]
    lanes = []
    for lane, label in enumerate(("BaseLuma", "FineLuma", "Chroma")):
        selected = [slot for slot in slots if slot[0] == lane]
        attempted = [slot for slot in selected if slot[1] not in (1, 2)]
        crc_decisions = [slot for slot in selected if slot[4] or slot[1] in (6, 7)]
        lanes.append({"lane": label, "fecAttemptedSlots": len(attempted),
            "fecFailures": sum(slot[1] == 3 for slot in attempted) if attempted else None,
            "explicitCrcDecisions": len(crc_decisions), "crcFailures": sum(slot[1] in (6, 7) for slot in crc_decisions) if crc_decisions else None,
            "acceptedSlots": sum(slot[5] for slot in selected), "note": "Null means stage not observed; CRC denominator includes only explicit decisions"})
    accepted = sum(row["acceptedBlocks"] for row in rows)
    if name in CASES[:3]:
        require(published and accepted == 15 * len(rows), "Clean/identity schedule failed full decode and recovery")
    if name == "bootstrap-conflict":
        require(not published and accepted == 0 and all(row["bootstrapErasure"] == 18 for row in rows), "Bootstrap conflict was not rejected")
    if name == "marker-plus-one":
        # DecodeUnifiedBootstrap performs Unified admission after the generic
        # locator and replaces its accepted status with InvalidGeometry.
        require(not published and accepted == 0 and all(not row["bootstrapAccepted"] and row["bootstrapErasure"] == 12 and row["geometry"] == 4 for row in rows), "Known strict geometry failure did not reproduce")
    if name == "shift-right-one":
        require(not published and accepted == 0, "One-pixel shift was admitted unexpectedly")
    if name == "neutral-chroma":
        require(lanes[2]["acceptedSlots"] == 0 and any(slot[1] == 2 for slot in slots), "Neutral chroma did not exercise lane rejection")
    if name == "local-freshness-mix":
        require(len(reached) == len(rows) and all(row["freshnessCurrent"] == [0, 1, 1, 1, 1, 1, 1, 1, 1] for row in rows), "Local mixed patch did not exercise freshness rejection")
    counters = report["stageCounters"]
    require(counters["outerConflictRejections"] == counters["outerResourceRejections"] == counters["staleResultDrops"] == 0, "Unexpected Receiver conflict/resource/stale result")
    require(counters["outerPeakActiveDecoderCount"] <= counters["outerActiveDecoderLimit"] and
            counters["outerPeakReservedDecoderBytes"] <= counters["outerTotalDecoderByteLimit"] and counters["resultQueueHighWater"] <= 2, "Resource high-water violation")
    outer_observed = summary["diagnostics"]["stages"]["outerReceive"]["calls"] > 0
    outer = {key: value for key, value in counters.items() if key.startswith("outer")} if outer_observed else None
    result = {"scenario": name, "observations": len(rows), "acceptedBlocksAcrossAllDemodObservations": accepted,
        "bootstrap": {"acceptedFrames": sum(row["bootstrapAccepted"] for row in rows), "erasureCodeHistogram": histogram(row["bootstrapErasure"] for row in rows)},
        "geometryStatusCodeHistogram": histogram(row["geometry"] for row in rows),
        "rawUnifiedFrameErasureCodeHistogram": histogram(row["frameErasure"] for row in rows),
        "freshness": {"observedFrames": len(reached), "staleRegionObservations": sum(9 - sum(row["freshnessCurrent"]) for row in reached) if reached else None},
        "lanes": lanes, "outer": outer, "publish": flags, "independentFileVerification": independent,
        "functionalRecovery": "PASS" if published else "NOT_RECOVERED", "knownGeometryFailureReproduced": name == "marker-plus-one",
        "acceptedPayloadAgreement": "ALL_ACCEPTED_MATCH_CLEAN_SAME_ORDINAL" if accepted else "NO_ACCEPTED_PAYLOADS",
        "receiverTelemetryObservationsThroughCompletion": report["unifiedTelemetry"]["observations"],
        "preFecBer": None, "preFecBerReason": "No pre-FEC bit-truth comparison in this bounded tool",
        "syntheticPtsSpanSeconds": summary["syntheticPtsSpanSeconds"], "liveChannelGoodput": None,
        "stageTimingCalls": {key: value["calls"] for key, value in summary["diagnostics"]["stages"].items()}}
    return {"result": result, "rows": normalized_rows, "transforms": normalized_transforms, "counters":
            {key: value for key, value in counters.items() if not key.endswith("TimeTotal100ns")}}


def analyze(fixture_root, suite, output):
    require(not output.exists(), "Analysis output must be new")
    require(not output.is_relative_to(fixture_root) and not output.is_relative_to(suite) and
            not fixture_root.is_relative_to(output) and not suite.is_relative_to(output), "Analysis must be disjoint from its inputs")
    fixture = load_fixture(fixture_root)
    suite_before = tree_identity(suite)
    identity = read_json(suite / "INPUT_IDENTITY.json")
    require(identity["fixtureIdentity"] == fixture["identity"] and identity["scenarios"] == list(CASES) and
            identity["observations"] == 76 and identity["trials"] == 2, "Suite/input identity mismatch")
    require(read_json(suite / "RUN_STATUS.json")["status"] == "EXECUTED_NOT_YET_ANALYZED", "Suite did not complete")
    results = []
    for name in CASES:
        trials = []
        for trial in (1, 2):
            truth = read_rows(suite / f"trial-{trial:02}" / "clean" / "frames.jsonl")
            require(len(truth) == 4 and len({(row["sessionTag"], row["frameSequence"]) for row in truth}) == 4,
                    "Clean frame identities must be four distinct observed identities")
            trials.append(case_analysis(suite / f"trial-{trial:02}" / name, name, fixture, truth))
        require(trials[0] == trials[1], f"Non-timing deterministic replay mismatch: {name}")
        result = trials[0]["result"]
        result["repeatability"] = "EXACT_MATCH_EXCLUDING_PROCESSING_CLOCKS"
        results.append(result)
    require(tree_identity(suite) == suite_before and tree_identity(fixture_root) == fixture["identity"], "Input/evidence changed during analysis")
    require(not output.exists(), "Analysis output must be new")
    output.mkdir()
    write_json(output / "analysis.json", {"schema": "PixelBridge.Step3A.Analysis.1", "status": "LOCAL_STEP3A_COMPLETE",
        "fullStep3Status": "PARTIAL", "fieldStatus": "NOT_RUN", "classification": "SyntheticOfflineDiagnostic",
        "scenarios": results, "observationsAcrossTwoTrials": 76, "deterministicCases": 9,
        "knownGeometryAdmissionFixed": False, "statefulVideoCodecMatrix": "NOT_RUN", "realScreen": "NOT_RUN",
        "sourceBytes": 512, "receiverCoverageNote": "DirectRepeat tiny source completes in first usable frame. Later demod observations are not Receiver dedup or multi-frame recovery tests.",
        "timingNote": "PTS is a fixed synthetic ordinal schedule, not measured sender time. Durations are inclusive processing timings, not channel speed.",
        "split": "SYNTHETIC_DEVELOPMENT_ONLY_NO_HOLDOUT", "liveChannelGoodput": None, "simulatedVerifiedGoodput": None})
    write_json(output / "INPUT_EVIDENCE_IDENTITY.json", {"suite": str(suite), "identity": suite_before})
    return results


def main():
    require(os.name == "nt", "Windows only")
    require(blake3(b"").hexdigest() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262", "Independent BLAKE3 KAT failed")
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run")
    run.add_argument("--exe", required=True, type=local_path)
    run.add_argument("--fixture", required=True, type=local_path)
    run.add_argument("--output", required=True, type=local_path)
    analysis = commands.add_parser("analyze")
    analysis.add_argument("--fixture", required=True, type=local_path)
    analysis.add_argument("--suite", required=True, type=local_path)
    analysis.add_argument("--output", required=True, type=local_path)
    arguments = parser.parse_args()
    if arguments.command == "run":
        run_suite(arguments.exe, arguments.fixture, arguments.output)
    else:
        for result in analyze(arguments.fixture, arguments.suite, arguments.output):
            print(result["scenario"], result["functionalRecovery"], result["acceptedBlocksAcrossAllDemodObservations"], result["repeatability"])


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, TypeError, OSError, RecursionError, subprocess.TimeoutExpired) as failure:
        print(f"Step3-A stopped; preserve all evidence: {failure}", file=sys.stderr)
        sys.exit(1)
