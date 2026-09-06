"""Serial, create-only supervisor for the opt-in deterministic Pass-0 probe."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("new_root", type=Path)
    parser.add_argument("--build-root", type=Path, default=Path("build-unified-release"))
    parser.add_argument("--modes", nargs="+", choices=["clean"] + list("01234567"), default=["clean", "3"])
    args = parser.parse_args()
    root = args.new_root.resolve()
    root.mkdir(exist_ok=False)
    repository = Path(__file__).resolve().parents[2]
    build = args.build_root.resolve()
    executable = build / "tests/PBApplication/Release/PBUnifiedWindowTransitionProbe.exe"
    executable_digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repository, text=True).strip()
    diff = subprocess.check_output(["git", "diff", "--binary"], cwd=repository)
    (root / "source.diff").write_bytes(diff)
    source_paths = ["apps/common/local_desktop_runtime.cpp", "apps/common/local_desktop_runtime.h",
                    "apps/common/unified_window_transition_probe.inc", "apps/common/sender_carousel_scheduler.cpp",
                    "apps/common/sender_carousel_scheduler.h", "tests/PBApplication/unified_window_transition_probe.cpp",
                    "tests/PBApplication/test_unified_sender_scheduler.cpp", "tests/PBApplication/CMakeLists.txt",
                    "tests/UnifiedRemoteGate/extract_g21_phase_loss.py", "tests/UnifiedRemoteGate/run_g21_transition_probe.py"]
    source_hashes = {}
    for relative in source_paths:
        data = (repository / relative).read_bytes()
        copy = root / "source-snapshot" / relative
        copy.parent.mkdir(parents=True, exist_ok=True)
        copy.write_bytes(data)
        source_hashes[relative] = hashlib.sha256(data).hexdigest()
    environment = dict(os.environ)
    environment["PATH"] = str(build / "vcpkg_installed/x64-windows/bin") + os.pathsep + environment["PATH"]
    results = []
    for mode in args.modes:
        command = [str(executable), str(root / ("case-" + mode)), mode]
        started = time.monotonic()
        timed_out = False
        with (root / (mode + ".stdout.txt")).open("x") as output, (root / (mode + ".stderr.txt")).open("x") as error:
            try:
                process = subprocess.run(command, cwd=repository, env=environment, stdout=output, stderr=error, timeout=240)
                exit_code = process.returncode
            except subprocess.TimeoutExpired:
                exit_code = None
                timed_out = True
        result_path = root / ("case-" + mode) / "result.json"
        result = json.loads(result_path.read_text()) if result_path.is_file() else None
        results.append({"command": command, "elapsedSeconds": time.monotonic() - started,
                        "exitCode": exit_code, "watchdogTerminated": timed_out, "result": result})
        print(json.dumps({"mode": mode, "exitCode": exit_code, "result": result}), flush=True)
        # Different loss phases are bounded diagnostic controls, not larger live
        # staircase tiers. Preserve all requested phase outcomes even after a miss.
    executable_unchanged = hashlib.sha256(executable.read_bytes()).hexdigest() == executable_digest
    report = {"schema": "PixelBridge.G21.TransitionSupervisor.1", "gitCommit": head,
              "uncommittedTrackedDiffSha256": hashlib.sha256(diff).hexdigest(),
              "probeExecutableSha256": executable_digest, "probeExecutableUnchanged": executable_unchanged,
              "sourceHashes": source_hashes,
              "deadlineSecondsPerCase": 240, "results": results,
              "allPassed": executable_unchanged and all(row["exitCode"] == 0 and row["result"] and row["result"]["passed"] for row in results)}
    with (root / "summary.json").open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    raise SystemExit(0 if report["allPassed"] else 1)


if __name__ == "__main__":
    main()
