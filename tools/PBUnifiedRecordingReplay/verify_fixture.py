"""Bounded headless integration checks; never passes source truth to replay."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--new-root", type=Path, required=True)
    args = parser.parse_args()
    args.new_root.mkdir(exist_ok=False)
    media = args.fixture / "fixture.mkv"
    source_hash = hashlib.sha256((args.fixture / "fixture-source.bin").read_bytes()).hexdigest()
    checks = []

    def run(name, path, mode="--replay", expected=0):
        output = args.new_root / name
        command = [str(args.tool.resolve()), mode, str(path.resolve()), str(output.resolve())]
        result = subprocess.run(command, capture_output=True, timeout=120)
        (args.new_root / f"{name}.log").write_bytes(result.stdout + result.stderr)
        assert result.returncode == expected, (name, result.returncode, result.stdout, result.stderr)
        report = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        checks.append({"name": name, "command": command, "exitCode": result.returncode})
        return output, report

    traces = []
    for name, mode in (("diagnostics-on", "--replay"), ("diagnostics-off", "--replay-no-diagnostics")):
        output, report = run(name, media, mode)
        assert report["error"] == "" and report["reachedEof"] and report["publishedAndReopened"]
        assert report["frames"] == 3 and report["fieldStatus"] == "NOT_RUN"
        assert report["liveChannelGoodput"] is None
        receiver = report["receiverReport"]
        assert receiver["unifiedTelemetry"]["uniqueVisualFps"] is None
        assert all(receiver["publish"][field] for field in ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified"))
        recovered = output / "output" / "fixture-source.bin"
        assert hashlib.sha256(recovered.read_bytes()).hexdigest() == source_hash
        rows = [json.loads(line) for line in (output / "frames.jsonl").read_text().splitlines()]
        assert len(rows) == 3 and len({(r["sessionTag"], r["frameSequence"]) for r in rows}) == 1
        assert all(row["bootstrapAccepted"] for row in rows)
        traces.append([{k: v for k, v in row.items() if k != "processingQpc100ns"} for row in rows])
    assert traces[0] == traces[1], "Diagnostics changed admitted pixel observations"

    content = media.read_bytes()
    for name, damaged in (("bad-header", b"not-matroska"), ("truncated-first-frame", content[:len(content) // 8]),
                          ("truncated-tail", content[:-512])):
        path = args.new_root / f"{name}.mkv"
        path.write_bytes(damaged)
        output, report = run(name, path, expected=1)
        assert report["error"] and not report["reachedEof"]
        if name != "truncated-tail":
            assert not report["publishedAndReopened"]
    _, missing = run("missing", args.new_root / "absent.mkv", expected=1)
    assert missing["frames"] == 0 and missing["error"]

    existing = args.new_root / "diagnostics-on"
    before = hashlib.sha256((existing / "summary.json").read_bytes()).hexdigest()
    result = subprocess.run([str(args.tool.resolve()), "--replay", str(media.resolve()), str(existing.resolve())],
                            capture_output=True, timeout=30)
    assert result.returncode == 1
    assert hashlib.sha256((existing / "summary.json").read_bytes()).hexdigest() == before
    checks.append({"name": "existing-root-refused", "exitCode": result.returncode})
    (args.new_root / "checks.json").write_text(json.dumps({"status": "PASS", "checks": checks,
        "sourceSha256": source_hash, "diagnosticsParity": True}, indent=2), encoding="utf-8")
    print(f"PASS: {len(checks)} checks; diagnostics on/off exact non-timing trace parity; source SHA256 {source_hash}")


if __name__ == "__main__":
    main()
