"""Step1 local preparation and POST-STOP audit. Never a decoder input adapter.

All outputs are create-only. No display/capture/input/network APIs or background
jobs are used. Package startup is explicit, bounded and offscreen only.
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import zipfile

BASE = "4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a"
PROFILE_HASH = "312c7832854f8710719ee2d0e44e920e7270b48638044eaa4b6af9ee24b1cb8b"
MAX_FILES = 65536
MAX_FILE = 128 * 1024 * 1024
MAX_PACKAGE = 512 * 1024 * 1024
MAX_EVIDENCE = 64 * 1024 * 1024
MAX_RECORD = 65536
MAX_FRAMES = 131072
MAX_TIME = 1800 * 10**9
SEGMENT_BYTES = 8 * 1024 * 1024
EXCLUDED = {"docs/PHASE1_GATE_REPORT.md", "30Hz_Remote.mkv"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def canonical(value):
    return (json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"), allow_nan=False) + "\n").encode("ascii")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate JSON key")
        result[key] = value
    return result


def parse_json(data):
    return json.loads(data, object_pairs_hook=unique_object, parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))


def no_links(path):
    path = Path(path).absolute()
    for item in (path, *path.parents):
        if item.exists() or item.is_symlink():
            facts = item.lstat()
            require(not stat.S_ISLNK(facts.st_mode) and not (getattr(facts, "st_file_attributes", 0) & 1024), "Reparse/symlink path rejected")
    return path


def safe_relative(name):
    require(isinstance(name, str) and 0 < len(name) <= 240, "Invalid relative path")
    require("\\" not in name and ":" not in name and not name.startswith("/"), "Noncanonical path")
    parts = name.split("/")
    for part in parts:
        require(part not in ("", ".", "..") and not part.endswith((".", " ")), "Traversal/ambiguous path")
        require(not any(ord(character) < 32 or character in '<>"|?*' for character in part), "Invalid path character")
        require(not re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part), "Reserved path")
    return PurePosixPath(name)


def hash_file(path, maximum=MAX_FILE):
    path = no_links(path)
    before = path.stat()
    require(path.is_file() and before.st_size <= maximum, "File size/type limit")
    digest = hashlib.sha256()
    count = 0
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            count += len(chunk)
            require(count <= maximum, "Growing file exceeds limit")
            digest.update(chunk)
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) and count == before.st_size, "File changed during hashing")
    return {"size": count, "sha256": digest.hexdigest()}


def read_json(path, maximum=8 * 1024 * 1024):
    path = no_links(path)
    require(path.stat().st_size <= maximum, "JSON size limit")
    return parse_json(path.read_bytes().decode("utf-8-sig"))


def write_new(path, value):
    path = no_links(path)
    data = value if isinstance(value, bytes) else canonical(value)
    with path.open("xb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def new_directory(path):
    path = no_links(path)
    path.mkdir(exist_ok=False)
    return path


def inventory(root):
    root = no_links(root)
    result = []
    for directory, names, files in os.walk(root, followlinks=False):
        for name in names:
            no_links(Path(directory) / name)
        for name in files:
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            safe_relative(relative)
            result.append({"path": relative, **hash_file(path)})
            require(len(result) <= MAX_FILES, "File count limit")
    require(sum(item["size"] for item in result) <= MAX_PACKAGE, "Aggregate byte limit")
    result.sort(key=lambda item: item["path"])
    require(len({item["path"].casefold() for item in result}) == len(result), "Case-fold path collision")
    return result


def run(command, *, cwd=None, timeout=30, environment=None):
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    result = subprocess.run([str(item) for item in command], cwd=cwd, timeout=timeout, env=environment,
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=creationflags)
    require(result.returncode == 0, f"Command failed ({result.returncode}): {command[0]}\n{result.stderr.decode('utf-8', errors='replace')[:2048]}")
    return result.stdout


def git(root, *arguments):
    return run(["git", "--no-optional-locks", *arguments], cwd=root)


def source_inventory(repository):
    repository = no_links(repository)
    require(git(repository, "rev-parse", "HEAD").decode().strip() == BASE, "Source base drift")
    tracked = git(repository, "ls-files", "-z").decode("utf-8").strip("\0").split("\0")
    untracked = git(repository, "ls-files", "--others", "--exclude-standard", "-z").decode("utf-8").strip("\0").split("\0")
    names = set(tracked)
    for name in filter(None, untracked):
        if name in EXCLUDED:
            continue
        require(name.startswith(("apps/", "libs/", "tests/", "tools/", "docs/")), f"Unreviewed untracked source: {name}")
        names.add(name)
    result = []
    for name in sorted(names - EXCLUDED):
        safe_relative(name)
        path = repository / name
        if path.exists():
            result.append({"path": name, **hash_file(path)})
    require(len(result) <= MAX_FILES and sum(item["size"] for item in result) <= MAX_PACKAGE, "Source inventory limit")
    return {"schema": "PixelBridge.Step1.SourceInventory.1", "baseCommit": BASE,
            "baseTree": git(repository, "rev-parse", "HEAD^{tree}").decode().strip(), "files": result,
            "excludedUserArtifacts": sorted(EXCLUDED)}


def freeze_source(args):
    repository = Path(args.repository)
    before = source_inventory(repository)
    root = new_directory(args.output)
    write_new(root / "source-inventory.json", before)
    write_new(root / "tracked.diff", git(repository, "diff", "--binary", "HEAD", "--", ".", ":(exclude)docs/PHASE1_GATE_REPORT.md"))
    with zipfile.ZipFile(root / "source-snapshot.zip", "x", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
        for item in before["files"]:
            archive.write(repository / item["path"], item["path"])
    require(source_inventory(repository) == before, "Source changed during freeze; partial evidence retained")
    seal = {"schema": "PixelBridge.Step1.SourceSeal.1", "baseCommit": BASE, "cleanRelease": False,
            "sourceFingerprintSha256": hash_file(root / "source-inventory.json")["sha256"],
            "snapshot": hash_file(root / "source-snapshot.zip"), "diff": hash_file(root / "tracked.diff"),
            "authority": "BasePlusDiffAndFullSourceSnapshot"}
    write_new(root / "source-seal.json", seal)
    return seal


def validate_entries(entries, *, maximum=MAX_PACKAGE):
    require(isinstance(entries, list) and len(entries) <= MAX_FILES, "Invalid file list")
    seen = set()
    total = 0
    for item in entries:
        require(set(item) == {"path", "size", "sha256"}, "Unexpected file identity fields")
        safe_relative(item["path"])
        require(item["path"].casefold() not in seen, "Duplicate/case-fold path")
        seen.add(item["path"].casefold())
        require(type(item["size"]) is int and 0 <= item["size"] <= MAX_FILE, "Invalid file length")
        require(isinstance(item["sha256"], str) and re.fullmatch("[0-9a-f]{64}", item["sha256"]), "Invalid SHA256")
        total += item["size"]
        require(total <= maximum, "Aggregate size limit")


def verify_zip(path, entries, *, extract_to=None):
    validate_entries(entries)
    expected = {item["path"]: item for item in entries}
    with zipfile.ZipFile(no_links(path)) as archive:
        require(len(archive.infolist()) == len(expected), "ZIP entry count mismatch")
        seen = set()
        for member in archive.infolist():
            safe_relative(member.filename)
            require(member.filename not in seen and member.filename in expected and not member.is_dir(), "Unexpected ZIP entry")
            require(not stat.S_ISLNK(member.external_attr >> 16) and member.flag_bits & 1 == 0, "ZIP link/encryption rejected")
            seen.add(member.filename)
            item = expected[member.filename]
            require(member.file_size == item["size"], "ZIP file size mismatch")
            digest = hashlib.sha256()
            count = 0
            with archive.open(member) as source:
                while chunk := source.read(1024 * 1024):
                    count += len(chunk)
                    require(count <= item["size"], "ZIP expanded past declared limit")
                    digest.update(chunk)
            require(count == item["size"] and digest.hexdigest() == item["sha256"], "ZIP payload hash mismatch")
        if extract_to is not None:
            root = new_directory(extract_to)
            for name in sorted(expected):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(name) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)


def validate_measurement_identity(identity, fingerprint):
    require(identity == {"schema": "PixelBridge.Step1.MeasurementBuildIdentity.1", "candidate": "M1",
            "authority": "InstrumentedExperiment", "baseCommit": BASE, "sourceFingerprintSha256": fingerprint,
            "wireChanged": False, "profileChanged": False}, "M1 build identity mismatch")
    require(re.fullmatch("[0-9a-f]{64}", fingerprint) is not None, "Unsealed experimental build")


def package(args):
    repository, build, source = map(Path, (args.repository, args.build, args.source_seal))
    source_seal = read_json(source / "source-seal.json")
    source_manifest = read_json(source / "source-inventory.json")
    require(source_inventory(repository) == source_manifest, "Workspace drift since source freeze")
    fingerprint = hash_file(source / "source-inventory.json")["sha256"]
    require(fingerprint == source_seal["sourceFingerprintSha256"], "Source seal mismatch")
    verify_zip(source / "source-snapshot.zip", source_manifest["files"])
    baseline = read_json(args.baseline_result)
    baseline_root = Path(baseline["packageDirectory"])
    require(hash_file(baseline_root / "package-manifest.json")["sha256"] == baseline["manifestSha256"], "B0 manifest drift")
    baseline_manifest = read_json(baseline_root / "package-manifest.json")
    require(hash_file(build / "vcpkg_installed/vcpkg/status")["sha256"] ==
            baseline_manifest["buildIdentity"]["vcpkg"]["installedStatusSha256"], "Dependency baseline drift")
    output = new_directory(args.output)
    root = new_directory(output / ("PB-Step1-M1-" + fingerprint[:12]))
    # Reuse the independently verified B0 license/runtime inventory, but bind
    # every DLL also present in the actual M1 build to exactly the same bytes.
    for item in baseline_manifest["files"]:
        relative = item["path"]
        safe_relative(relative)
        if not (relative.startswith(("Encoder/", "Decoder/", "licenses/")) or
                relative in ("THIRD_PARTY_NOTICES.txt", "unified-profile.json")):
            continue
        if relative.endswith(".exe"):
            continue
        require(hash_file(baseline_root / relative) == {"size": item["size"], "sha256": item["sha256"]}, "B0 payload drift")
        parts = relative.split("/", 1)
        if len(parts) == 2 and parts[0] in ("Encoder", "Decoder"):
            built = build / ("apps/PixelBridge" + parts[0]) / "Release" / parts[1]
            if built.is_file():
                require(hash_file(built) == hash_file(baseline_root / relative), "M1 DLL differs from recorded dependency reference")
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(baseline_root / relative, target)
    applications = []
    for role in ("Encoder", "Decoder"):
        executable = build / ("apps/PixelBridge" + role) / "Release" / ("PixelBridge" + role + ".exe")
        identity = parse_json(run([executable, "--measurement-build-identity"]))
        validate_measurement_identity(identity, fingerprint)
        legacy = parse_json(run([executable, "--build-identity"]))
        require(legacy["gitCommit"] == BASE and legacy["schema"] == "PixelBridge.ApplicationBuildIdentity.1", "Legacy identity drift")
        require(hashlib.sha256(run([executable, "--unified-profile"]).strip()).hexdigest() == PROFILE_HASH, "Profile drift")
        relative = role + "/" + executable.name
        shutil.copyfile(executable, root / relative)
        applications.append({"role": role, "path": relative, "identity": identity, "legacyIdentity": legacy, **hash_file(executable)})
    shutil.copyfile(build / "tools/Release/PBStep1SourceAudit.exe", root / "Encoder/PBStep1SourceAudit.exe")
    (root / "source").mkdir()
    for name in ("source-inventory.json", "source-seal.json", "source-snapshot.zip", "tracked.diff"):
        shutil.copyfile(source / name, root / "source" / name)
    write_new(root / "source/build-cache.txt", (build / "CMakeCache.txt").read_bytes())
    write_new(root / "source/dependencies.status", (build / "vcpkg_installed/vcpkg/status").read_bytes())
    for name in ("step1.py", "Test-PBStep1Package.ps1", "Start-Step1Gui.ps1", "New-Step1InputAudit.ps1"):
        shutil.copyfile(repository / "tools/PBRemoteThroughputStep1" / name, root / name)
    shutil.copyfile(repository / "docs/REMOTE_STEP1_MEASUREMENT_CONTRACT.md", root / "FIELD_OPERATIONS.md")
    shutil.copyfile(baseline_root / "SBOM.spdx.json", root / "dependency-reference.spdx.json")
    write_new(root / "dependency-provenance.json", {"schema": "PixelBridge.Step1.DependencyProvenance.1",
        "baselineManifestSha256": baseline["manifestSha256"], "buildIdentity": baseline_manifest["buildIdentity"],
        "scope": "Unchanged DLL/license reference only; not M1 application or field certification"})
    entries = inventory(root)
    manifest = {"schema": "PixelBridge.Step1.ExperimentalPackage.1", "candidate": "M1", "cleanRelease": False,
        "fieldStatus": "NOT_RUN", "sourceFingerprintSha256": fingerprint, "baseCommit": BASE,
        "profileSha256": PROFILE_HASH, "applications": applications, "files": entries}
    write_new(root / "package-manifest.json", manifest)
    archive = output / (root.name + ".zip")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as writer:
        for item in inventory(root):
            writer.write(root / item["path"], item["path"])
    seal = {"schema": "PixelBridge.Step1.ExperimentalPackageSeal.1", "manifest": hash_file(root / "package-manifest.json"),
        "archive": hash_file(archive, MAX_PACKAGE), "sourceFingerprintSha256": fingerprint, "authenticityClaim": False}
    seal_path = output / (root.name + ".seal.json")
    write_new(seal_path, seal)
    result = {"schema": "PixelBridge.Step1.PackageResult.1", "packageDirectory": str(root.absolute()),
        "archivePath": str(archive.absolute()), "sealPath": str(seal_path.absolute()),
        "manifestSha256": seal["manifest"]["sha256"], "archiveSha256": seal["archive"]["sha256"],
        "sourceFingerprintSha256": fingerprint}
    write_new(output / "package-result.json", result)
    verify_package(root, seal_path, result["manifestSha256"], archive)
    require(source_inventory(repository) == source_manifest, "Source drift during packaging")
    return result


def verify_package(root, seal_path, expected_manifest, archive=None):
    root = no_links(root)
    manifest_identity = hash_file(root / "package-manifest.json")
    require(manifest_identity["sha256"] == expected_manifest, "Pinned manifest mismatch")
    manifest = read_json(root / "package-manifest.json")
    seal = read_json(seal_path)
    require(manifest["schema"] == "PixelBridge.Step1.ExperimentalPackage.1" and manifest["candidate"] == "M1" and
            manifest["cleanRelease"] is False and manifest["baseCommit"] == BASE and manifest["fieldStatus"] == "NOT_RUN", "Package identity/scope mismatch")
    require(seal["schema"] == "PixelBridge.Step1.ExperimentalPackageSeal.1" and seal["authenticityClaim"] is False, "Seal schema mismatch")
    require(seal["manifest"] == manifest_identity and seal["sourceFingerprintSha256"] == manifest["sourceFingerprintSha256"], "Package seal mismatch")
    validate_entries(manifest["files"])
    require([item for item in inventory(root) if item["path"] != "package-manifest.json"] == manifest["files"], "Missing/extra/tampered package payload")
    require(manifest["profileSha256"] == PROFILE_HASH and hash_file(root / "unified-profile.json")["sha256"] == PROFILE_HASH, "Profile mismatch")
    require({item["role"] for item in manifest["applications"]} == {"Encoder", "Decoder"} and len(manifest["applications"]) == 2, "Application role mismatch")
    for item in manifest["applications"]:
        require(item["path"] == item["role"] + "/PixelBridge" + item["role"] + ".exe", "Executable role/path mismatch")
        validate_measurement_identity(item["identity"], manifest["sourceFingerprintSha256"])
        require(hash_file(root / item["path"]) == {"size": item["size"], "sha256": item["sha256"]}, "Application hash mismatch")
    source_manifest = read_json(root / "source/source-inventory.json")
    source_seal = read_json(root / "source/source-seal.json")
    require(hash_file(root / "source/source-inventory.json")["sha256"] == manifest["sourceFingerprintSha256"] ==
            source_seal["sourceFingerprintSha256"] and source_manifest["baseCommit"] == BASE, "Source fingerprint mismatch")
    require(hash_file(root / "source/source-snapshot.zip") == source_seal["snapshot"] and
            hash_file(root / "source/tracked.diff") == source_seal["diff"], "Source archive/diff mismatch")
    verify_zip(root / "source/source-snapshot.zip", source_manifest["files"])
    if archive is not None:
        require(hash_file(archive, MAX_PACKAGE) == seal["archive"], "Archive seal mismatch")
        verify_zip(archive, [*manifest["files"], {"path": "package-manifest.json", **manifest_identity}])
    return {"schema": "PixelBridge.Step1.PackageVerification.1", "verified": True, "packagedExecutablesRun": False,
            "authenticityClaim": False, "sourceFingerprintSha256": manifest["sourceFingerprintSha256"]}


@contextlib.contextmanager
def immutable_read_lease(path):
    if os.name != "nt":
        yield
        return
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x08000000, None)
    require(handle != ctypes.c_void_p(-1).value, "Cannot acquire immutable read lease")
    try:
        yield
    finally:
        require(kernel.CloseHandle(handle) != 0, "Source lease close failed")


def dual_digest(path):
    import blake3
    path = no_links(path)
    with immutable_read_lease(path):
        before = path.stat()
        require(before.st_size <= MAX_EVIDENCE, "Step1 fixed-file audit limit 64 MiB")
        sha, blake = hashlib.sha256(), blake3.blake3()
        count, segments = 0, []
        with path.open("rb") as source:
            while chunk := source.read(SEGMENT_BYTES):
                count += len(chunk)
                require(count <= MAX_EVIDENCE, "Input grew beyond limit")
                sha.update(chunk)
                blake.update(chunk)
                segments.append({"rawBytes": len(chunk), "rawBlake3": blake3.blake3(chunk).hexdigest()})
        after = path.stat()
        require((before.st_size, before.st_ino, before.st_dev, before.st_mtime_ns) ==
                (after.st_size, after.st_ino, after.st_dev, after.st_mtime_ns) and count == after.st_size, "Input mutated")
    return {"bytes": count, "sha256": sha.hexdigest(), "blake3": blake.hexdigest(), "segments": segments,
            "fileId": str(before.st_ino), "device": str(before.st_dev), "mtimeNs": str(before.st_mtime_ns)}


def validate_ledger(ledger):
    require(ledger["schema"] == "PixelBridge.Step1.SourceLedger.1" and ledger["complete"] is True, "Incomplete source ledger")
    require(type(ledger["rawBytes"]) is int and 0 <= ledger["rawBytes"] <= MAX_EVIDENCE and
            type(ledger["encodedBytes"]) is int and 0 <= ledger["encodedBytes"] <= MAX_EVIDENCE, "Ledger byte limit")
    require(re.fullmatch("[0-9a-f]{64}", ledger["blake3"]) is not None, "Ledger digest invalid")
    require(isinstance(ledger["compressionIdentity"], str) and 0 < len(ledger["compressionIdentity"]) < 512, "Compression identity invalid")
    require(len(ledger["segments"]) == (ledger["rawBytes"] + SEGMENT_BYTES - 1) // SEGMENT_BYTES, "Segment coverage gap")
    raw = encoded = 0
    for ordinal, segment in enumerate(ledger["segments"]):
        require(segment["ordinal"] == ordinal and segment["rawOffset"] == raw, "Segment offset/ordinal mismatch")
        require(segment["rawBytes"] == min(SEGMENT_BYTES, ledger["rawBytes"] - raw) and
                type(segment["encodedBytes"]) is int and 0 < segment["encodedBytes"] <= segment["rawBytes"], "Segment byte mismatch")
        require(segment["codec"] in ("Raw", "Zstandard"), "Unknown codec")
        for name in ("rawBlake3", "encodedBlake3"):
            require(re.fullmatch("[0-9a-f]{64}", segment[name]) is not None, "Segment digest invalid")
        if segment["codec"] == "Raw":
            require(segment["rawBytes"] == segment["encodedBytes"] and segment["rawBlake3"] == segment["encodedBlake3"], "RAW ledger mismatch")
        raw += segment["rawBytes"]
        encoded += segment["encodedBytes"]
    require(raw == ledger["rawBytes"] and encoded == ledger["encodedBytes"], "Ledger totals mismatch")


def audit_input(args):
    ledger = read_json(args.ledger, MAX_RECORD)
    validate_ledger(ledger)
    digest = dual_digest(args.source)
    require(digest["bytes"] == ledger["rawBytes"] and digest["blake3"] == ledger["blake3"], "Source/ledger identity mismatch")
    require(digest["segments"] == [{"rawBytes": item["rawBytes"], "rawBlake3": item["rawBlake3"]} for item in ledger["segments"]], "Raw segment digest mismatch")
    result = {"schema": "PixelBridge.Step1.InputAudit.1", "source": str(Path(args.source).absolute()), "identity": digest,
              "ledger": ledger, "ledgerFile": hash_file(args.ledger), "independentDigestImplementation": "Python hashlib + blake3; immutable Win32 read lease"}
    write_new(args.output, result)
    return result


def verify_run(root):
    root = no_links(root)
    seal = read_json(root / "evidence-seal.json", MAX_RECORD)
    require(seal["schema"] == "PixelBridge.Step1.RunEvidenceSeal.1" and seal["complete"] is True and
            seal["failure"] == "None" and seal["payloadSideChannel"] is False, "Incomplete/failed run evidence")
    require(hash_file(root.parent / "entry.json", MAX_RECORD)["sha256"] == seal["entrySha256"], "GUI entry identity missing/tampered")
    entry = read_json(root.parent / "entry.json", MAX_RECORD)
    require(entry["schema"] == "PixelBridge.Step1.GuiEntry.1" and entry["role"] == seal["role"] and
            entry["manualStartRequired"] is True and entry["automaticInput"] is False, "GUI entry scope mismatch")
    entries = [{"path": item["path"], "size": item["bytes"], "sha256": item["sha256"]} for item in seal["files"]]
    validate_entries(entries, maximum=MAX_EVIDENCE)
    require({item["path"] for item in entries} == ({"events.jsonl", "final.json", "source-ledger.json"} if seal["role"] == "Encoder" else {"events.jsonl", "final.json"}), "Run evidence coverage mismatch")
    for item in entries:
        require(hash_file(root / item["path"], MAX_EVIDENCE) == {"size": item["size"], "sha256": item["sha256"]}, "Run evidence hash mismatch")
    report = read_json(root / "final.json", MAX_RECORD)
    require(report["schema"] == "PixelBridge.RunReport.3" and report["role"] == seal["role"], "Run role/schema mismatch")
    require(report["profile"] == "PB-Unified-SC6-V3" and report["visualLayoutVersion"] == 10 and
            report["visualProfileId"] == 0x5042554E49534333, "Run profile mismatch")
    measurement = report["measurement"]
    require(measurement["schema"] == "PixelBridge.Step1.Measurement.1" and measurement["evidenceFailure"] == "None" and
            measurement["terminalSucceeded"] is True and not report["errorDetail"], "Run/cleanup failed")
    validate_measurement_identity(measurement["build"], measurement["build"]["sourceFingerprintSha256"])
    require(entry["build"] == measurement["build"], "Run/entry build mismatch")
    if report["role"] == "Encoder":
        require(report["state"] == "Stopped" and report["preparation"]["complete"] is True and
                report["preparation"]["sourceStabilityVerified"] is True and not report["preparation"]["resumed"], "Sender run is not a fresh stable stopped sample")
        ledger = read_json(root / "source-ledger.json", MAX_RECORD)
        validate_ledger(ledger)
        require(ledger == measurement["sourceLedger"], "Runtime source ledger drift")
    report["_entry"] = entry
    return report


def checked_timing(report):
    measurement = report["measurement"]
    require(report["state"] == "Completed" and report["role"] == "Decoder", "Receiver not complete")
    require(measurement["clock"] == "process-steady" and measurement["unit"] == "nanoseconds" and
            measurement["origin"] == "startAccepted" and measurement["mainEnd"] == "finalReopenVerified", "Timing convention mismatch")
    require(measurement["receiverTimingEligible"] is True and measurement["unavailableReason"] is None, "Receiver timing unavailable")
    publish, recovery = report["publish"], report["recovery"]
    require(all(publish.get(name) is True for name in ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published")), "Final integrity/publish chain missing")
    require(not recovery["resumeLoaded"] and not publish["recoveredAfterRename"], "Resume is not full-run performance")
    required = ["startAccepted", "captureReady", "firstVisualObservation", "firstAcceptedBootstrap", "firstControlAccepted",
                "wholeDigestVerified", "finalRenameSucceeded", "finalReopenVerified", "terminal"]
    if report["fileBytes"]:
        required += ["firstUsefulEquation", "lastSegmentStored"]
    times = measurement["milestones"]
    require(all(type(times.get(name)) is int and 0 <= times[name] <= MAX_TIME for name in required), "Missing/invalid timing")
    pairs = [("firstVisualObservation", "firstAcceptedBootstrap"), ("firstAcceptedBootstrap", "firstControlAccepted"),
             ("firstControlAccepted", "wholeDigestVerified"), ("captureReady", "finalReopenVerified"),
             ("wholeDigestVerified", "finalRenameSucceeded"), ("finalRenameSucceeded", "finalReopenVerified"), ("finalReopenVerified", "terminal")]
    if report["fileBytes"]:
        pairs += [("firstUsefulEquation", "lastSegmentStored"), ("lastSegmentStored", "wholeDigestVerified")]
    require(all(times[before] <= times[after] for before, after in pairs), "Invalid milestone partial order")
    elapsed = times["finalReopenVerified"]
    require(times["startAccepted"] == 0 and 0 < elapsed <= MAX_TIME and measurement["elapsedNanoseconds"] == elapsed, "Invalid main duration")
    require(recovery["verifiedRawBytes"] == report["fileBytes"] and type(recovery["verifiedEncodedSegmentBytes"]) is int, "Missing byte coverage")
    return elapsed


def correlate(args):
    package_result = read_json(args.package_result)
    package_root = Path(package_result["packageDirectory"])
    verify_package(package_root, package_result["sealPath"], package_result["manifestSha256"])
    package_manifest = read_json(package_root / "package-manifest.json")
    sender = verify_run(args.sender_run)
    receiver = verify_run(args.receiver_run)
    for endpoint in (sender, receiver):
        application = next(item for item in package_manifest["applications"] if item["role"] == endpoint["role"])
        require(endpoint["measurement"]["build"] == application["identity"] and
                endpoint["_entry"]["executable"]["sha256"] == application["sha256"] and
                endpoint["_entry"]["executable"]["size"] == application["size"], "Run executing binary does not match sealed package")
    identity = read_json(args.input_audit)
    before = read_json(args.sender_source_before)
    after = read_json(args.sender_source_after)
    for audit in (identity, before, after):
        require(audit["schema"] == "PixelBridge.Step1.InputAudit.1", "Input audit missing")
        validate_ledger(audit["ledger"])
        require(audit["ledger"] == identity["ledger"], "Sender source encoding differs from fixed input")
        require(all(audit["identity"][name] == identity["identity"][name] for name in ("bytes", "sha256", "blake3")), "Sender source differs from fixed input")
    require(before["identity"] == after["identity"] and before["source"] == after["source"], "Sender input changed across the run")
    operator = read_json(args.operator_record, MAX_RECORD)
    require(identity["schema"] == "PixelBridge.Step1.InputAudit.1", "Input audit missing")
    require(operator["schema"] == "PixelBridge.Step1.OperatorRecord.1" and operator["startupScenario"] in ("late-join", "cold-start") and
            operator["recording"] in ("on", "off") and operator["manualOperation"] is True and
            operator["topologyRechecked"] is True and operator["protectedScreenUntouched"] is True, "Operator conditions incomplete")
    require(sender["measurement"]["build"] == receiver["measurement"]["build"], "Cross-endpoint candidate mismatch")
    require(sender["sessionId"] == receiver["sessionId"] and sender["sessionTag"] == receiver["sessionTag"], "Cross-endpoint session mismatch")
    require(sender["measurement"]["sourceLedger"] == identity["ledger"], "Fixed source or encoded bytes changed")
    require(sender["configuredLogicalFps"] == operator["configuredHz"], "Configured Hz mismatch")
    elapsed = checked_timing(receiver)
    require(os.path.normcase(os.path.abspath(args.published_file)) == os.path.normcase(os.path.abspath(receiver["publish"]["finalPath"])),
            "External audit must reopen the receiver-reported authoritative final path")
    digest = dual_digest(args.published_file)
    for name in ("bytes", "sha256", "blake3"):
        require(digest[name] == identity["identity"][name], "Published/source external digest mismatch")
    require(receiver["fileBytes"] == identity["identity"]["bytes"] and receiver["recovery"]["verifiedEncodedSegmentBytes"] == identity["ledger"]["encodedBytes"], "Verified byte numerator mismatch")
    first = receiver["measurement"]["firstAcceptedBootstrap"]
    require(first["sessionTag"] == str(sender["sessionTag"]), "First observed identity was another session")
    wanted = first["sessionTag"], first["frameSequence"]
    match, count, previous, last_time = None, 0, None, -1
    with (Path(args.sender_run) / "events.jsonl").open("rb") as source:
        while line := source.readline(MAX_RECORD + 1):
            require(len(line) <= MAX_RECORD and line.endswith(b"\n"), "Truncated/overlong sender record")
            record = parse_json(line)
            if record["kind"] != "submittedFrame":
                require(record["kind"] == "sample", "Unexpected sender record")
                continue
            count += 1
            require(count <= MAX_FRAMES, "Sender identity record limit")
            key = record["sessionTag"], record["frameSequence"]
            require(record["sessionTag"] == str(sender["sessionTag"]), "Mixed sender identity")
            require(all(isinstance(record[name], str) and re.fullmatch(r"0|[1-9][0-9]{0,19}", record[name]) for name in
                        ("sessionTag", "frameSequence", "carouselPass", "segmentOrdinal", "cyclePosition")), "Sender integer identity malformed")
            require(all(int(record[name]) <= 2**64 - 1 for name in ("sessionTag", "frameSequence", "carouselPass", "segmentOrdinal", "cyclePosition")), "Identity overflow")
            sequence = int(record["frameSequence"])
            require(previous is None or sequence > previous, "Sender sequence not strictly increasing")
            require(type(record["submittedOffsetNanoseconds"]) is int and max(0, last_time) <= record["submittedOffsetNanoseconds"] <= MAX_TIME, "Sender clock invalid")
            previous, last_time = sequence, record["submittedOffsetNanoseconds"]
            if key == wanted:
                match = record
    require(count == sender["measurement"]["submittedRecords"] == sender["measurement"]["drainedRecords"] ==
            sender["scheduler"]["submittedLogicalFrames"], "Incomplete sender trace coverage")
    require(match is not None, "Exact sender identity match unavailable; no interpolation permitted")
    if operator["startupScenario"] == "late-join":
        require(int(match["carouselPass"]) > 0, "No later-Carousel first observed phase proof")
    result = {"schema": "PixelBridge.Step1.PostStopCorrelation.1", "sampleEligible": True,
        "authority": "PostStopExactIdentityAndFullFileAudit", "operatorRecord": operator,
        "measurementBuild": sender["measurement"]["build"], "firstObservedSenderPhase": match,
        "actualClickSenderPhase": None, "crossHostTimestampSubtraction": False,
        "elapsedNanoseconds": elapsed, "verifiedRawGoodputBytesPerSecond": digest["bytes"] * 10**9 / elapsed,
        "verifiedEncodedGoodputBytesPerSecond": identity["ledger"]["encodedBytes"] * 10**9 / elapsed,
        "externalDualDigest": digest, "inputAudit": hash_file(args.input_audit),
        "senderSourceBefore": hash_file(args.sender_source_before), "senderSourceAfter": hash_file(args.sender_source_after),
        "senderFinal": hash_file(Path(args.sender_run) / "final.json"), "receiverFinal": hash_file(Path(args.receiver_run) / "final.json")}
    write_new(args.output, result)
    return result


def startup(args):
    result = read_json(args.package_result)
    root = new_directory(args.output)
    source_root = Path(result["packageDirectory"])
    verify_package(source_root, result["sealPath"], result["manifestSha256"], result["archivePath"])
    manifest = read_json(source_root / "package-manifest.json")
    unpacked = root / "unpacked"
    verify_zip(result["archivePath"], [*manifest["files"], {"path": "package-manifest.json", **hash_file(source_root / "package-manifest.json")}], extract_to=unpacked)
    environment = dict(os.environ)
    for name in list(environment):
        if name.startswith(("QT_", "QML")):
            del environment[name]
    windows = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    environment["PATH"] = str(windows / "System32") + os.pathsep + str(windows)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    checks = []
    for application in manifest["applications"]:
        executable = unpacked / application["path"]
        for option in ("--version", "--build-identity", "--measurement-build-identity", "--unified-profile", "--gui-smoke"):
            output = run([executable, option], timeout=25, environment=environment)
            write_new(root / (application["role"] + option + ".txt"), output)
            if option == "--measurement-build-identity":
                validate_measurement_identity(parse_json(output), manifest["sourceFingerprintSha256"])
            elif option == "--build-identity":
                require(parse_json(output) == application["legacyIdentity"], "Legacy runtime identity differs")
            elif option == "--unified-profile":
                require(hashlib.sha256(output.strip()).hexdigest() == PROFILE_HASH, "Runtime profile mismatch")
            elif option == "--gui-smoke":
                require(b"PASS" in output and b"offscreen" in output, "Offscreen smoke did not pass")
            checks.append({"role": application["role"], "option": option, "passed": True})
    verify_package(unpacked, result["sealPath"], result["manifestSha256"], result["archivePath"])
    summary = {"schema": "PixelBridge.Step1.PackageStartup.1", "passed": True, "checks": checks,
               "offscreenOnly": True, "nativeOrRemoteTested": False, "pristineWindowsVmTested": False}
    write_new(root / "summary.json", summary)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command, names in {
        "freeze-source": ["repository", "output"], "package": ["repository", "build", "source-seal", "baseline-result", "output"],
        "verify-package": ["package", "seal", "expected-manifest"], "audit-input": ["source", "ledger", "output"],
        "verify-run": ["run"], "correlate": ["package-result", "sender-run", "receiver-run", "input-audit", "sender-source-before", "sender-source-after", "operator-record", "published-file", "output"],
        "startup": ["package-result", "output"]}.items():
        sub = commands.add_parser(command)
        for name in names:
            sub.add_argument("--" + name, required=True)
        if command == "verify-package":
            sub.add_argument("--archive")
    args = parser.parse_args(argv)
    try:
        if args.command == "verify-package":
            result = verify_package(args.package, args.seal, args.expected_manifest, args.archive)
        elif args.command == "verify-run":
            report = verify_run(args.run)
            result = {"verified": True, "role": report["role"], "runId": report["runId"]}
        else:
            result = {"freeze-source": freeze_source, "package": package, "audit-input": audit_input,
                      "correlate": correlate, "startup": startup}[args.command](args)
        print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
        return 0
    except (ValueError, OSError, KeyError, TypeError, zipfile.BadZipFile, subprocess.SubprocessError, ImportError) as error:
        print(f"Step1 FAILED: {error}; existing/partial evidence retained", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
