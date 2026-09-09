"""Five narrow negative checks of the standalone G1 tools; no product mutation."""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def snapshot(path):
    return {str(item.relative_to(path)): hashlib.sha256(item.read_bytes()).hexdigest() for item in path.rglob("*") if item.is_file()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output-new", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_new.resolve()
    if root.exists() or not root.is_relative_to(args.repo.resolve() / "artifacts"):
        raise RuntimeError("Guard evidence root must be new and workspace-local")
    root.mkdir(parents=True, exist_ok=False)
    source = args.repo / "tools/PBUnifiedGeometryG1"
    executable = args.build / "Release/PBUnifiedGeometryG1.exe"
    checks = []

    def expect_failure(name, command):
        result = subprocess.run(command, cwd=args.repo, capture_output=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
        with (root / (name + ".stdout")).open("xb") as stream:
            stream.write(result.stdout)
        with (root / (name + ".stderr")).open("xb") as stream:
            stream.write(result.stderr)
        assert result.returncode != 0, name
        checks.append({"name": name, "command": list(map(str, command)), "exit": result.returncode, "passed": True})

    expect_failure("invalid-cli", [executable, "--unexpected"])
    expect_failure("missing-recording", [executable, "--recording", root / "absent-input.mkv"])
    existing = root / "existing-evidence"
    existing.mkdir()
    (existing / "sentinel.txt").write_text("Preserve existing evidence", encoding="utf-8")
    before = snapshot(existing)
    expect_failure("existing-evidence", [sys.executable, source / "run_checks.py", "--repo", args.repo, "--build", args.build, "--output-new", existing])
    assert snapshot(existing) == before
    generated = root / "existing-generated"
    sentinel = generated / "include/pbmodulation/unified_visual.h"
    sentinel.parent.mkdir(parents=True)
    sentinel.write_text("Preserve generated-source sentinel", encoding="utf-8")
    before = snapshot(generated)
    expect_failure("existing-generated", [sys.executable, source / "prepare_reference.py", "--repo", args.repo, "--output", generated])
    assert snapshot(generated) == before
    altered = root / "altered-trace"
    for name, filename in [("fixtures-01", "fixtures.jsonl"), ("fixtures-final-02", "fixtures.jsonl"), ("recording-01", "recording-prefix.jsonl"), ("recording-final-02", "recording-prefix.jsonl")]:
        target = altered / "runs" / name / filename
        target.parent.mkdir(parents=True)
        target.write_bytes((args.run / "runs" / name / filename).read_bytes())
    path = altered / "runs/fixtures-final-02/fixtures.jsonl"
    lines = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]
    next(row for row in lines if row["type"] == "fixture")["candidate"]["acceptedBlocks"] = 99
    path.write_text("\n".join(json.dumps(row) for row in lines) + "\n", encoding="utf-8")
    report = altered / "invalid-result.json"
    expect_failure("altered-trace-count", [sys.executable, source / "analyze_results.py", "--repo", args.repo, "--run", altered,
                   "--fixture-run", "fixtures-final-02", "--recording-run", "recording-final-02", "--output-new", report])
    invalid = json.loads(report.read_text(encoding="utf-8"))
    assert not invalid["allChecksPassed"] and invalid["verdict"] == "EVIDENCE_INVALID"
    with (root / "checks.json").open("x", encoding="utf-8") as stream:
        json.dump({"checks": checks, "allPassed": True}, stream, indent=2)
    print(json.dumps({"guardChecks": len(checks), "allPassed": True}))


if __name__ == "__main__":
    main()
