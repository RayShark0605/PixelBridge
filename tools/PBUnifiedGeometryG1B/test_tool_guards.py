"""Small no-window negative checks. All derived fixtures/logs are create-only."""
import argparse
import copy
import json
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--output-new", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve(strict=True)
    root = args.output_new.resolve()
    if root.exists() or not root.is_relative_to(repo / "artifacts"):
        raise RuntimeError("Guard evidence root must be new under artifacts")
    root.mkdir(parents=True, exist_ok=False)
    tool = repo / "tools/PBUnifiedGeometryG1B"
    executable = args.build.resolve(strict=True) / "Release/PBUnifiedGeometryG1B.exe"
    checks = []

    def run_failure(name, command, sentinel=None, result=None):
        before = sentinel.read_bytes() if sentinel else None
        with (root / f"{name}.stdout").open("xb") as stdout, (root / f"{name}.stderr").open("xb") as stderr:
            process = subprocess.Popen(command, cwd=repo, stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                code = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError(f"Guard timeout: {name}")
        passed = code == 1 and (sentinel is None or sentinel.read_bytes() == before)
        if result is not None:
            record = json.loads(result.read_text(encoding="utf-8"))
            passed = passed and record["verdict"] == "EVIDENCE_INVALID" and record["allChecksPassed"] is False
        checks.append({"name": name, "passed": passed, "exit": code, "command": command})

    run_failure("invalid-cli", [str(executable), "--invalid"])
    run_failure("missing-media", [str(executable), "--recording", str(root / "does-not-exist.mkv")])
    existing = root / "existing-output"
    existing.mkdir()
    sentinel = existing / "sentinel.txt"
    sentinel.write_bytes(b"Preserve existing evidence\n")
    run_failure("existing-run-root", [sys.executable, str(tool / "run_checks.py"), "--repo", str(repo), "--build", str(args.build), "--output-new", str(existing)], sentinel)
    generated = root / "existing-generated"
    generated.mkdir()
    generated_sentinel = generated / "local_desktop_decode_g1b.cpp"
    generated_sentinel.write_bytes(b"Do not overwrite generated history\n")
    run_failure("existing-generated-source", [sys.executable, str(tool / "prepare_reference.py"), "--repo", str(repo), "--output", str(generated)], generated_sentinel)

    rows = [json.loads(line) for line in args.fixtures.read_text(encoding="utf-8").splitlines()]
    fixture_index = next(index for index, row in enumerate(rows) if row["type"] == "fixture")

    def bad_trace(name, transform):
        altered = copy.deepcopy(rows)
        transform(altered[fixture_index])
        path = root / f"{name}.jsonl"
        with path.open("x", encoding="utf-8") as stream:
            for row in altered:
                stream.write(json.dumps(row) + "\n")
        return path

    corruptions = [
        ("wrong-block-count", lambda row: row.update(cpuAcceptedBlocks=99)),
        ("wrong-edge-reading", lambda row: row["probe"]["edges"][0].__setitem__(1, row["probe"]["edges"][0][1] + 0.25)),
        ("wrong-edge-count", lambda row: row["probe"]["edges"].pop()),
        ("nonfinite-edge", lambda row: row["probe"]["edges"][0].__setitem__(1, float("nan"))),
    ]
    for name, transform in corruptions:
        path = bad_trace(name, transform)
        result = root / f"{name}-result.json"
        run_failure(name, [sys.executable, str(tool / "analyze_results.py"), "--repo", str(repo), "--fixtures", str(path),
                           "--recording", str(args.recording), "--output-new", str(result)], result=result)
    oversized = root / "oversized.jsonl"
    oversized.write_bytes(b" " * (1024 * 1024 + 1))
    result = root / "oversized-result.json"
    run_failure("oversized-evidence", [sys.executable, str(tool / "analyze_results.py"), "--repo", str(repo), "--fixtures", str(oversized),
                                       "--recording", str(args.recording), "--output-new", str(result)], result=result)
    result_sentinel = root / "existing-analysis.json"
    result_sentinel.write_bytes(b"Do not overwrite an existing analysis\n")
    run_failure("existing-analysis", [sys.executable, str(tool / "analyze_results.py"), "--repo", str(repo), "--fixtures", str(args.fixtures),
                                      "--recording", str(args.recording), "--output-new", str(result_sentinel)], result_sentinel)
    summary = {"schema": "PixelBridge.G1B.ToolGuards.1", "checks": checks, "allPassed": all(row["passed"] for row in checks)}
    with (root / "checks.json").open("x", encoding="utf-8") as stream:
        json.dump(summary, stream, indent=2)
    print(json.dumps({"checks": len(checks), "allPassed": summary["allPassed"]}))
    if not summary["allPassed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
