"""Create-only evidence seal; --verify reads files and performs no child replay."""
from __future__ import annotations
import argparse
import ast
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile
from analyze import analyze
from analyze_history import analyze as analyze_history

REPO = Path(__file__).resolve().parents[2]
CHANGED = {"apps/common/local_desktop_runtime.cpp", "apps/common/recorded_pixel_replay.h",
    "apps/common/recorded_pixel_replay.inc", "docs/README.md", "docs/REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md"}
ADDED = {"apps/common/receiver_decision_trace.h", "docs/REMOTE_RECEIVER_DECISION_DIAGNOSTICS_2026-09-08.md"} | {
    f"tools/PBReceiverDecisionProbe/{name}" for name in ["CMakeLists.txt", "main.cpp", "test_decisions.cpp", "run.py",
        "analyze.py", "analyze_history.py", "seal.py", "README.md"]}
PROTECTED = "076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306"
ZIP_NAME = "RECEIVER_DECISION_EVIDENCE.zip"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while data := stream.read(1024 * 1024):
            digest.update(data)
    return digest.hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path, data):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(data, stream, indent=2, ensure_ascii=False)
        stream.write("\n")


def item(path, root):
    return {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size, "sha256": sha(path)}


def audit(root):
    baseline = read(root / "context/SOURCE_BASELINE.json")
    before = {entry["path"]: entry for entry in baseline["files"]}
    paths = set(subprocess.check_output(["git", "ls-files", "-c", "-o", "--exclude-standard", "-z"], cwd=REPO).decode("utf-8").split("\0")) - {"", "30Hz_Remote.mkv"}
    require(paths - before.keys() == ADDED and not (before.keys() - paths), "unexpected source addition/removal")
    files = [item(REPO / name, REPO) for name in sorted(paths)]
    changed = {entry["path"] for entry in files if entry["path"] in before and entry["sha256"] != before[entry["path"]]["sha256"]}
    require(changed == CHANGED, f"unexpected existing source delta: {changed}")
    require(sha(REPO / "docs/PHASE1_GATE_REPORT.md") == PROTECTED, "protected file changed")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO).decode().strip()
    require(head == baseline["head"], "HEAD changed")
    require(not subprocess.check_output(["git", "diff", "--cached", "--name-only"], cwd=REPO), "index is not empty")
    subprocess.run(["git", "diff", "--check", "--", *sorted(CHANGED)], cwd=REPO, check=True, capture_output=True)
    for name in sorted(ADDED | CHANGED):
        data = (REPO / name).read_text(encoding="utf-8-sig")
        if name.endswith(".py"):
            ast.parse(data, filename=name)
        require(not any(line.endswith((" ", "\t")) for line in data.splitlines()), f"trailing whitespace in {name}")
    build_inputs = read(root / "identity/BUILD_INPUT_IDENTITY.json")
    require(build_inputs["globalVcpkgHeaders"] == [], "global injected dependency headers")
    for entry in build_inputs["sourceAndHeaders"] + build_inputs["baseLibraries"]:
        require(sha(Path(entry["path"])) == entry["sha256"], f"build input changed: {entry['path']}")
    for entry in read(root / "identity/RUNTIME_IDENTITY.json"):
        require(sha(Path(entry["path"])) == entry["sha256"], "runtime changed")
    for entry in read(root / "context/INPUT_IDENTITY.json"):
        require(sha(Path(entry["path"])) == entry["sha256"], "historical input changed")
    parity = analyze(root)
    require(parity == read(root / "PARITY_ANALYSIS.json"), "saved parity result differs")
    require(analyze_history(REPO) == read(root / "HISTORICAL_ATTRIBUTION.json"), "saved historical result differs")
    for name in ["build-04", "tests-03", "raw-off", "raw-on"]:
        process = read(root / f"logs/{name}.process.json")
        require(process["returnCode"] == 0 and process["failures"] == [] and process["jobAssignedBeforeResume"], f"required child failed: {name}")
    source = {"schema": "PixelBridge.ReceiverDecisionSource.1", "head": head, "files": files,
        "changedPreexistingFiles": sorted(changed), "addedFiles": sorted(ADDED), "oldFilesUnchanged": len(before) - len(changed),
        "protectedSha256": PROTECTED, "indexEmpty": True}
    return source, parity


def verify(root):
    source, parity = audit(root)
    require(source == read(root / "SOURCE_IDENTITY_FINAL.json"), "final source inventory differs")
    manifest = read(root / "FINAL_MANIFEST.json")
    archive = root / ZIP_NAME
    require(sha(archive) == manifest["archive"]["sha256"] and archive.stat().st_size == manifest["archive"]["bytes"], "archive identity")
    expected = {entry["path"] for entry in manifest["files"]}
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()} - {ZIP_NAME, "FINAL_MANIFEST.json", "POST_SEAL_VERIFY.json"}
    require(actual == expected, "evidence member set changed")
    with zipfile.ZipFile(archive) as zipped:
        require(len(zipped.namelist()) == len(expected) and set(zipped.namelist()) == expected, "archive member set")
        for entry in manifest["files"]:
            path = (root / entry["path"]).resolve()
            require(path.is_relative_to(root) and item(path, root) == entry, f"member changed: {entry['path']}")
            data = zipped.read(entry["path"])
            require(len(data) == entry["bytes"] and hashlib.sha256(data).hexdigest() == entry["sha256"], "archive member hash")
    total = sum(path.stat().st_size for path in root.rglob("*") if path.is_file())
    require(total < 128 * 1024**2, "evidence budget exceeded")
    return {"status": "PASS", "sourceFiles": len(source["files"]), "members": len(expected), "archiveBytes": archive.stat().st_size,
        "archiveSha256": sha(archive), "evidenceBytes": total, "normalPixelObservations": parity["normalPixelObservations"], "fieldStatus": "NOT_RUN"}


def seal(root):
    source, parity = audit(root)
    with zipfile.ZipFile(root / "source-delta.zip", "x", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(CHANGED | ADDED):
            archive.write(REPO / name, name)
    with zipfile.ZipFile(root / "context/pre-edit-files.zip") as previous:
        differences = []
        for name in sorted(CHANGED):
            old = previous.read(name).decode("utf-8-sig").splitlines(keepends=True)
            new = (REPO / name).read_text(encoding="utf-8-sig").splitlines(keepends=True)
            differences.extend(difflib.unified_diff([line.rstrip("\r\n") + "\n" for line in old], new, fromfile="before/" + name, tofile="after/" + name))
        with (root / "context/final-scoped.patch").open("x", encoding="utf-8", newline="\n") as stream:
            stream.write("".join(differences))
    write(root / "SOURCE_IDENTITY_FINAL.json", source)
    write(root / "FINAL_STATUS.json", {"localGoal": "COMPLETE", "diagnosticContract": "OUTPUT_ONLY_FIXED_15_SLOTS",
        "targetedTests": {"families": 6, "admissionCases": 10, "status": "PASS", "pixelObservations": 0},
        "rawParity": parity["status"], "normalPixelObservations": 60, "normalPixelBudget": 60,
        "newCodecRuns": 0, "fullRecordingReplays": 0, "realScreenTests": 0, "senderChanged": False,
        "wholeStep3": "PARTIAL", "fieldStatus": "NOT_RUN", "step4": "NOT_STARTED", "speedupClaim": None,
        "gitHeadUnchanged": True, "indexEmpty": True, "protectedSha256": PROTECTED,
        "review": "Scoped source diff reviewed for output-only decisions, null/error meaning, fixed storage, bounds and unchanged admission/publish gates; no known Critical/High finding",
        "limitations": ["Admission tests are not pixel certification", "Fixed codec remains NOT_RECOVERED",
            "Failure diagnostic writer/byte bounds tested locally; no extra WARP fault-injection run", "No product EXE/field release certification"]})
    files = [item(path, root) for path in sorted(root.rglob("*")) if path.is_file()]
    with zipfile.ZipFile(root / ZIP_NAME, "x", zipfile.ZIP_DEFLATED) as archive:
        for entry in files:
            archive.write(root / entry["path"], entry["path"])
    write(root / "FINAL_MANIFEST.json", {"schema": "PixelBridge.ReceiverDecisionSeal.1", "files": files,
        "archive": item(root / ZIP_NAME, root), "excluded": ["FINAL_MANIFEST.json", "POST_SEAL_VERIFY.json", ZIP_NAME]})
    result = verify(root)
    write(root / "POST_SEAL_VERIFY.json", result)
    return verify(root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    print(json.dumps(verify(root) if args.verify else seal(root), indent=2))
