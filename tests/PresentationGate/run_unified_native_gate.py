"""One bounded, explicitly selected G20 DISPLAY2 case; never runs a matrix implicitly.

Decoder gets only its output directory and physical ROI dimensions. Source bytes,
hashes, Session IDs and sender reports are not provided to the receiving process.
The supervisor reads both reports only for lifecycle/evidence and post-publish audit.
No input injection, display setting changes, or external window manipulation.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time

import blake3


CASES = {
    "native-15-1x": (1920, 1080, 15, 25, False),
    "native-1-1x": (1920, 1080, 1, 45, False),
    "native-60-1x": (1920, 1080, 60, 25, False),
    "native-15-075x": (1440, 810, 15, 25, False),
    "native-15-1125x": (2160, 1215, 15, 25, False),
    "native-15-letterbox": (2240, 1120, 15, 25, False),
    "native-pause-resume": (1920, 1080, 15, 25, True),
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def save_new(path, value):
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, ensure_ascii=True)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digests(path):
    sha256 = hashlib.sha256()
    b3 = blake3.blake3()
    size = 0
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha256.update(block)
            b3.update(block)
            size += len(block)
    return {"bytes": size, "sha256": sha256.hexdigest(), "blake3": b3.hexdigest()}


def catalog(executable):
    result = subprocess.run([str(executable), "--list-monitors"], capture_output=True,
                            creationflags=subprocess.CREATE_NO_WINDOW, timeout=15, check=True)
    data = json.loads(result.stdout)
    monitors = {item["deviceName"]: item for item in data["monitors"]}
    require(len(monitors) == data["monitorCount"], "ambiguous monitor identities")
    right = monitors[r"\\.\DISPLAY2"]
    left = monitors[r"\\.\DISPLAY1"]
    require(right["resolution"] == {"width": 2560, "height": 1440}, "right resolution changed")
    require(right["physicalRect"]["left"] >= left["physicalRect"]["right"], "DISPLAY2 is not strictly right of protected screen")
    require(right["rotation"] == "Identity", "unexpected right-screen rotation")
    return data


def wait_marker(process, marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        require(process.poll() is None, f"Encoder exited {process.returncode} before {marker.name}")
        if marker.is_file():
            # DiagnosticFile publishes a complete create-only file, but tolerate
            # an in-progress write without altering or retrying the native case.
            try:
                return read_json(marker)
            except json.JSONDecodeError:
                pass
        time.sleep(0.1)
    raise TimeoutError(f"deadline waiting for {marker}")


def audit_cadence(root, fps):
    ready = read_json(root / "encoder/ready.json")
    final = read_json(root / "encoder/active-final.json")
    elapsed_ms = final["nativeGate"]["steadyMilliseconds"] - ready["nativeGate"]["steadyMilliseconds"]
    submitted = final["scheduler"]["submittedLogicalFrames"] - ready["scheduler"]["submittedLogicalFrames"]
    # Absolute tick deadlines (G09): one interval boundary plus one frame being
    # built while a snapshot is sampled. Never demand legacy min-submit dwell.
    upper = math.ceil(elapsed_ms * fps / 1000) + 2
    require(elapsed_ms >= 15000 and 0 < submitted <= upper, "absolute-clock submission budget violated")
    rows = [json.loads(line) for line in (root / "encoder/samples.jsonl").read_text(encoding="utf-8").splitlines()]
    require(all(row["nativeGate"]["pendingFrames"] <= 1 for row in rows), "pending frame queue exceeded one")
    require(all(right["nativeGate"]["frameSequence"] >= left["nativeGate"]["frameSequence"] for left, right in zip(rows, rows[1:])),
            "observed frame identity went backwards")
    return {"basis": "G09 absolute logical tick deadlines; completed-submit jitter is not minimum dwell",
            "elapsedMilliseconds": elapsed_ms, "submittedFrames": submitted, "upperBoundWithSnapshotBoundary": upper,
            "legacyBelowNominalSubmitIntervals": final["nativeGate"]["logicalDwellViolationCount"],
            "boundedPendingQueue": True, "monotonicFrameIdentity": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--catalog-executable", type=Path, required=True)
    parser.add_argument("--run-directory", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--diagnostic-decoder", action="store_true",
                        help="Observe a failed case with bounded read-only demod diagnostics; not the default acceptance run")
    args = parser.parse_args()
    require(os.name == "nt", "native gate requires Windows")
    worker = args.worker.resolve(strict=True)
    monitor_tool = args.catalog_executable.resolve(strict=True)
    root = args.run_directory.resolve()
    root.mkdir(exist_ok=False)
    width, height, fps, seconds, pause = CASES[args.case]
    seal = digests(worker)
    catalog_before = catalog(monitor_tool)
    save_new(root / "monitor-before.json", catalog_before)
    source = root / "csprng-256k.bin"
    with source.open("xb") as output:
        output.write(os.urandom(256 * 1024))
    source_hashes = digests(source)
    output_directory = root / "received"
    output_directory.mkdir()
    encoder_command = [str(worker), "--encoder", str(source), str(root / "encoder"),
                       str(width), str(height), str(fps), str(seconds), "1" if pause else "0"]
    decoder_command = [str(worker), "--diagnose-decoder" if args.diagnostic_decoder else "--decoder", str(output_directory), str(root / "decoder"),
                       str(width), str(height), str(seconds - 3)]
    provenance = {"case": args.case, "worker": str(worker), "workerDigests": seal,
                  "sourceDigests": source_hashes, "sourceGenerator": "Windows os.urandom CSPRNG",
                  "encoderCommand": encoder_command, "decoderCommand": decoder_command,
                  "decoderReceivesSourceOrOracle": False, "pixelPath": "Native D3D11 window -> desktop capture Auto -> production Unified Receiver",
                  "inputAutomation": False, "displaySettingChanges": False, "protectedMonitor": r"\\.\DISPLAY1",
                  "experimentMonitor": r"\\.\DISPLAY2", "twoTimesScaleExecuted": False,
                  "diagnosticDecoder": args.diagnostic_decoder}
    save_new(root / "provenance.json", provenance)
    processes = []
    files = []
    started = time.monotonic()
    outcome = {"case": args.case, "passed": False, "diagnosticDecoder": args.diagnostic_decoder}

    def launch(role, command):
        stdout = (root / f"{role}.stdout.log").open("xb")
        stderr = (root / f"{role}.stderr.log").open("xb")
        files.extend([stdout, stderr])
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        processes.append(process)
        save_new(root / f"{role}-started.json", {"pid": process.pid, "command": command})
        return process

    try:
        encoder = launch("encoder", encoder_command)
        wait_marker(encoder, root / "encoder" / ("paused.json" if pause else "ready.json"), 18)
        require(catalog(monitor_tool) == catalog_before, "topology changed before receiver start")
        decoder = launch("decoder", decoder_command)
        decoder_exit = decoder.wait(timeout=seconds + 15)
        # Sender completion never depends on Receiver state; its fixed deadline
        # closes the run even if the Decoder has already published or failed.
        encoder_exit = encoder.wait(timeout=seconds + 20)
        outcome.update(encoderExit=encoder_exit, decoderExit=decoder_exit)
        require(encoder_exit == 0 and decoder_exit == 0, "native worker failure; preserve original reports")
        encode = read_json(root / "encoder/active-final.json")
        decode = read_json(root / "decoder/final.json")
        publish = decode["publish"]
        require(decode["state"] == "Completed" and all(publish[key] for key in
                ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published")), "authoritative publish gates not met")
        final_path = Path(publish["finalPath"]).resolve(strict=True)
        require(final_path.parent == output_directory and final_path.name == source.name, "unexpected final target")
        final_hashes = digests(final_path)
        require(source_hashes == final_hashes and source.read_bytes() == final_path.read_bytes(), "independent byte-exact verification failed")
        require(publish["wholeFileDigest"] == source_hashes["blake3"] == encode["sourceWholeFileDigest"], "in-band versus independent BLAKE3 mismatch")
        cadence = audit_cadence(root, fps)
        require(encode["nativeGate"]["rawSegmentCount"] == 1 and encode["nativeGate"]["zstdSegmentCount"] == 0, "CSPRNG fixture did not take RAW path")
        require(encode["nativeGate"]["clientWidth"] == width and encode["nativeGate"]["clientHeight"] == height, "actual client dimensions mismatch")
        require(encode["scheduler"]["submittedLogicalFrames"] > 2, "insufficient logical cadence observation")
        if pause:
            before = read_json(root / "encoder/paused.json")
            after = read_json(root / "encoder/paused-end.json")
            require(after["nativeGate"]["steadyMilliseconds"] - before["nativeGate"]["steadyMilliseconds"] >= 3800, "pause observation too short")
            require(before["nativeGate"]["frameSequence"] == after["nativeGate"]["frameSequence"], "paused identity changed")
            require(read_json(root / "encoder/matte.json")["neutralMatteMatched"], "screen matte evidence missing")
        outcome.update(passed=True, bytes=source_hashes["bytes"], finalDigests=final_hashes,
                       configuredLogicalFps=fps, observedSubmittedLogicalFps=encode["observedSubmittedLogicalFps"],
                       capture=decode["capture"], telemetry=decode["unifiedTelemetry"],
                       verifiedEncodedBytesPerUniqueFrame=decode["verifiedEncodedBytesPerUniqueFrame"],
                       publish=publish, controlSlotOccupancy=encode["scheduler"]["controlSlotOccupancy"],
                       pauseResume=pause, finalByteExact=True, cadence=cadence)
    except Exception as error:
        outcome["error"] = f"{type(error).__name__}: {error}"
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()  # Only an owned, bounded failed/timed-out child.
                process.wait(timeout=10)
        for file in files:
            file.close()
        outcome["wallSeconds"] = round(time.monotonic() - started, 3)
        outcome["workerUnchanged"] = digests(worker) == seal
        outcome["sourceUnchanged"] = digests(source) == source_hashes
        try:
            after = catalog(monitor_tool)
            save_new(root / "monitor-after.json", after)
            outcome["displayCatalogUnchanged"] = after == catalog_before
        except Exception as error:
            outcome["displayCatalogUnchanged"] = False
            outcome["catalogError"] = str(error)
        outcome["passed"] &= outcome["workerUnchanged"] and outcome["sourceUnchanged"] and outcome["displayCatalogUnchanged"]
        save_new(root / "summary.json", outcome)
    print(json.dumps(outcome, ensure_ascii=True), flush=True)
    return 0 if outcome["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
