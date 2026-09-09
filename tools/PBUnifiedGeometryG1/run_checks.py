"""Manual, bounded, no-window execution with create-only evidence roots."""
import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output-new", type=Path, required=True)
    parser.add_argument("--recording", type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve(strict=True)
    build = args.build.resolve(strict=True)
    output = args.output_new.resolve()
    if not output.is_relative_to(repo / "artifacts") or output.exists():
        raise RuntimeError("Evidence output must be a new directory under this workspace's artifacts")
    output.mkdir(parents=True, exist_ok=False)
    executable = build / "Release/PBUnifiedGeometryG1.exe"
    if args.recording is not None:
        commands = [("recording-prefix", [str(executable), "--recording", str(args.recording.resolve(strict=True))])]
    else:
        commands = [
            ("fixtures", [str(executable)]),
            ("fixed-canvas", [str(build / "Release/PBLocalDesktopBootstrapTests.exe"), "[fixed-canvas]", "--reporter", "console", "--rng-seed", "2092026"]),
            ("existing-geometry", [str(repo / "build-remote-step2-20260908-run01/tests/PBModulation/Release/PBUnifiedVisualCpuTests.exe"), "[point-downscale],[point-coverage],[remote-coverage]", "--reporter", "console", "--rng-seed", "2092026"]),
        ]
    results = []
    for name, command in commands:
        start = time.monotonic()
        stdout = output / (name + ".jsonl" if name in {"fixtures", "recording-prefix"} else name + ".stdout")
        stderr = output / (name + ".stderr")
        timed_out = False
        with stdout.open("xb") as out_stream, stderr.open("xb") as err_stream:
            process = subprocess.Popen(command, cwd=repo, stdout=out_stream, stderr=err_stream, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                exit_code = process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                exit_code = process.returncode
                timed_out = True
        oversized = stdout.stat().st_size > 1024 * 1024 or stderr.stat().st_size > 64 * 1024
        results.append({"name": name, "command": command, "exit": exit_code, "timedOut": timed_out, "oversizedOutput": oversized,
                        "seconds": time.monotonic() - start, "executableSha256": hashlib.sha256(Path(command[0]).read_bytes()).hexdigest()})
        print(json.dumps(results[-1], ensure_ascii=False))
        if exit_code or timed_out or oversized:
            break
    with (output / "commands.json").open("x", encoding="utf-8") as stream:
        json.dump(results, stream, indent=2)
    if len(results) != len(commands) or any(row["exit"] or row["timedOut"] or row["oversizedOutput"] for row in results):
        raise RuntimeError("Bounded run failed; partial output preserved")


if __name__ == "__main__":
    main()
