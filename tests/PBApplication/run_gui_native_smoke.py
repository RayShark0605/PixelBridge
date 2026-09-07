"""Explicit right-monitor-only 1 MB native GUI test, with no mouse/keyboard automation.

Decoder gets only output/ROI/test-log settings; it never receives source, digest,
Encoder cache or any payload IPC. A fixed Encoder deadline is independent of RX.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import blake3


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def hashes(path):
    sha256 = hashlib.sha256()
    blake = blake3.blake3()
    count = 0
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            sha256.update(block)
            blake.update(block)
            count += len(block)
    return {"bytes": count, "sha256": sha256.hexdigest(), "blake3": blake.hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--encoder", type=Path, required=True)
    parser.add_argument("--decoder", type=Path, required=True)
    parser.add_argument("--experiment-monitor", required=True)
    parser.add_argument("--protected-monitor", required=True)
    parser.add_argument("--evidence-directory", type=Path, required=True)
    args = parser.parse_args()
    root = args.evidence_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    output = root / "output"
    output.mkdir()
    source = root / "source-1MB.bin"
    source.write_bytes(os.urandom(1024 * 1024))
    original = hashes(source)
    proofs = {}
    processes = []
    handles = []
    result = {"schema": "PixelBridge.GuiNativeEndToEnd.1", "passed": False,
              "automaticInputEvents": False, "payloadChannel": "Actual right-monitor pixels only",
              "source": original, "encoderSeconds": 45, "decoderSeconds": 90}

    def start(role, executable, input_path, seconds):
        logs = root / role.lower()
        arguments = [str(executable.resolve()), "--gui-native-smoke", args.experiment_monitor, args.protected_monitor,
                     str(input_path), str(logs), str(seconds)]
        stdout = (root / (role + ".stdout.log")).open("xb")
        stderr = (root / (role + ".stderr.log")).open("xb")
        handles.extend([stdout, stderr])
        process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
        processes.append(process)
        result[role + "Launch"] = {"pid": process.pid, "arguments": arguments, "monotonic": time.monotonic()}
        return process

    try:
        for role, executable in [("Encoder", args.encoder), ("Decoder", args.decoder)]:
            probe = subprocess.run([str(executable.resolve()), "--build-identity"], capture_output=True, timeout=12)
            require(probe.returncode == 0, "Identity probe failed")
            proofs[role] = {"identity": json.loads(probe.stdout), "executable": hashes(executable)}
        require(proofs["Encoder"]["identity"]["gitCommit"] == proofs["Decoder"]["identity"]["gitCommit"], "Mixed binary commits")
        result["binaries"] = proofs
        decoder = start("Decoder", args.decoder, output, 90)
        ready = root / "decoder/capture-ready.json"
        deadline = time.monotonic() + 25
        while not ready.exists() and decoder.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        require(ready.exists() and decoder.poll() is None, "Receiver did not become capture-ready; Encoder was not started")
        require(json.loads(ready.read_bytes()) == {"captureStarted": True}, "Invalid capture-ready evidence")
        encoder = start("Encoder", args.encoder, source, 45)
        result["decoderExitCode"] = decoder.wait(timeout=105)
        # No receiver ACK or completion signal is sent to Encoder. Its own
        # fixed test clock expires independently after more Carousel passes.
        result["encoderExitCode"] = encoder.wait(timeout=65)
        for role in ("Encoder", "Decoder"):
            proof = json.loads((root / role.lower() / "native-smoke.json").read_bytes())
            require(proof["passed"] and proof["safetyHeld"] and not proof["automaticInputEvents"], role + " native UI proof failed")
            require(proof["gitCommit"] == proofs[role]["identity"]["gitCommit"], "Native proof identity changed")
        require(result["encoderExitCode"] == 0 and result["decoderExitCode"] == 0, "Native process failed")
        receive = json.loads((root / "decoder/run-report.json").read_bytes())
        send = json.loads((root / "encoder/run-report.json").read_bytes())
        publish = receive["publish"]
        require(receive["schema"] == "PixelBridge.RunReport.3" and receive["state"] == "Completed", "Receiver did not complete")
        require(all(publish[key] is True for key in ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published")), "Missing final publication gate")
        restored = Path(publish["finalPath"]).resolve(strict=True)
        require(restored.parent == output and restored.name == source.name, "Final path escaped expected output directory")
        require(receive["recovery"]["verifiedRawBytes"] == 1024 * 1024 and receive["recovery"]["largeOutputConfirmation"] == "NotRequired", "Unexpected admission/recovery result")
        recovered = hashes(restored)
        require(recovered == original and hashes(source) == original, "Independent reopened SHA-256/BLAKE3/size mismatch")
        require(publish["wholeFileDigest"] == original["blake3"], "In-band whole digest differs from independent BLAKE3")
        require(send["sessionId"] == receive["sessionId"] and send["runEndedUnixMilliseconds"] > receive["runEndedUnixMilliseconds"], "Sender did not outlive receiver completion")
        result.update({"passed": True, "outputPath": str(restored), "output": recovered,
                       "senderContinuedAfterReceiverMilliseconds": send["runEndedUnixMilliseconds"] - receive["runEndedUnixMilliseconds"],
                       "physicalEscKeyTested": False, "physicalRoiDragTested": False})
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        # Only test-owned PIDs can be terminated, and only after an error or
        # expired watchdog. Never enumerate or stop another PixelBridge run.
        terminated = []
        for process in processes:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
                terminated.append(process.pid)
        for handle in handles:
            handle.close()
        result["watchdogTerminatedOwnedPids"] = terminated
        (root / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
