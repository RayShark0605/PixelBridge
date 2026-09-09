"""Serial, create-only supervision. No codec, GUI, capture or network input."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/PBRemoteThroughputStep3B"))
from process_runner import invoke


def write_new(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False)
        stream.write("\n")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def checked_run(argv, log, timeout=300):
    result = invoke(argv, log, timeout, limit=4 * 1024 * 1024)
    write_new(log.with_suffix(".json"), result)
    if result.get("returnCode") != 0 or result.get("error") or result.get("failures"):
        raise RuntimeError(f"Child failed; preserve {log}; no automatic retry")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["build", "test", "parity"])
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--label", default="01")
    args = parser.parse_args()
    root, build = args.root.resolve(), args.build.resolve()
    if not (root / "context/SCOPE.json").is_file() or not args.label.isalnum():
        raise ValueError("Explicit initialized evidence root and simple label required")
    executable = build / "Release/PBReceiverDecisionProbe.exe"
    if args.action == "build":
        if not build.exists():
            checked_run([r"C:\Program Files\CMake\bin\cmake.exe", "-S", Path(__file__).parent, "-B", build,
                "-G", "Visual Studio 17 2022", "-A", "x64", f"-DPB_DECISION_REPO={REPO}",
                f"-DPB_DECISION_BASE_BUILD={REPO / 'build-remote-step2-20260908-run01'}"], root / f"logs/configure-{args.label}.log")
        checked_run([r"C:\Program Files\CMake\bin\cmake.exe", "--build", build, "--config", "Release", "--parallel", "2"], root / f"logs/build-{args.label}.log")
    elif args.action == "test":
        checked_run([build / "Release/PBReceiverDecisionTests.exe", root / f"tests/admission-{args.label}"], root / f"logs/tests-{args.label}.log", 60)
    else:
        executable = root / "runtime/PBReceiverDecisionProbe.exe"
        build_identity = json.loads((root / "identity/BUILD_INPUT_IDENTITY.json").read_text(encoding="utf-8"))
        for item in build_identity["sourceAndHeaders"] + build_identity["baseLibraries"]:
            if sha256(Path(item["path"])) != item["sha256"]:
                raise ValueError(f"Frozen build input changed: {item['path']}")
        runtime_identity = json.loads((root / "identity/RUNTIME_IDENTITY.json").read_text(encoding="utf-8"))
        for item in runtime_identity:
            if sha256(Path(item["path"])) != item["sha256"]:
                raise ValueError(f"Frozen runtime changed: {item['path']}")
        source = REPO / "artifacts/remote-step3b-20260908-run01/frozen-pixels/source.bgra"
        expected = "c9251f1458cd1739b12636bda3f13764bade05e664f0e3c646f8c554d8b6f7bd"
        if source.stat().st_size != 248832000 or sha256(source) != expected:
            raise ValueError("Original fixed pixel identity changed")
        write_new(root / "NORMAL_OBSERVATION_RESERVATION.json", {"observations": 60, "runs": 2,
            "perRun": 30, "source": str(source), "sourceSha256": expected, "exeSha256": sha256(executable),
            "note": "Budget reserved before first child; never rerun or resume this command in the same root"})
        for mode in ["off", "on"]:
            checked_run([executable, f"--{mode}", source, root / f"runs/{mode}"], root / f"logs/raw-{mode}.log")
        if sha256(source) != expected:
            raise ValueError("Source identity changed during parity")


if __name__ == "__main__":
    main()
