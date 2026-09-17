"""Read-only verifier integration negatives on a create-only derived package copy.

The original package/archive/seal are never edited. No packaged executable runs.
"""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import zipfile


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-directory", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--seal", type=Path, required=True)
    parser.add_argument("--evidence-directory", type=Path, required=True)
    parser.add_argument("--powershell", required=True)
    args = parser.parse_args()
    original = args.package_directory.resolve(strict=True)
    root = args.evidence_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    package = root / original.name
    shutil.copytree(original, package)
    manifest_path = package / "package-manifest.json"
    baseline = manifest_path.read_bytes()
    manifest = json.loads(baseline)
    verifier = Path(__file__).resolve().parents[2] / "tools/PBUnifiedRelease/Test-PBUnifiedPortablePackage.ps1"
    original_inventory = {str(path.relative_to(original)): digest(path) for path in original.rglob("*") if path.is_file()}
    archive_hash = digest(args.archive)
    seal_hash = digest(args.seal)
    results = []
    product_release = manifest["buildIdentity"]["releaseScope"] == "LocalProductRelease"

    def run(name, expected_error=None, archive=None, seal=None):
        command = [args.powershell, "-NoProfile", "-File", str(verifier), "-PackageDirectory", str(package)]
        if seal:
            command += ["-PackageSealPath", str(seal)]
        if archive:
            command += ["-ArchivePath", str(archive)]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        (root / (name + ".stdout.log")).write_text(result.stdout, encoding="utf-8")
        (root / (name + ".stderr.log")).write_text(result.stderr, encoding="utf-8")
        passed = result.returncode == 0 if expected_error is None else result.returncode != 0 and expected_error in result.stderr
        if expected_error is None and result.returncode == 0:
            proof = json.loads(result.stdout)
            passed = proof["verified"] and proof["packagedExecutablesRun"] is False
        results.append({"name": name, "passed": passed, "exitCode": result.returncode,
                        "expectedError": expected_error, "elapsedSeconds": time.monotonic() - started})
        print(f"{name}: {'PASS' if passed else 'FAIL'}", flush=True)
        if not passed:
            raise AssertionError(name + ": " + result.stderr[-2000:])

    def changed_manifest(name, change, error):
        altered = copy.deepcopy(manifest)
        change(altered)
        try:
            manifest_path.write_text(json.dumps(altered, ensure_ascii=False), encoding="utf-8")
            run(name, error)
        finally:
            manifest_path.write_bytes(baseline)

    def changed_product_file(name, relative, change, error):
        target_path = package / relative
        original_bytes = target_path.read_bytes()
        altered = copy.deepcopy(manifest)
        try:
            changed = change(original_bytes)
            if changed is None:
                target_path.unlink()
                altered["files"] = [entry for entry in altered["files"] if entry["path"] != relative]
            else:
                target_path.write_bytes(changed)
                for entry in altered["files"]:
                    if entry["path"] == relative:
                        entry.update(size=len(changed), sha256=hashlib.sha256(changed).hexdigest())
            # Preserve the producer's canonical path order. Rebind the payload
            # inventory so the semantic product checks, not byte tamper, decide.
            altered["packageFileCount"] = len(altered["files"])
            altered["packagePayloadBytes"] = sum(entry["size"] for entry in altered["files"])
            canonical = "".join(f"{e['path']}\0{e['size']}\0{e['sha256']}\n" for e in altered["files"])
            altered["packagePayloadFingerprintSha256"] = hashlib.sha256(canonical.encode()).hexdigest()
            manifest_path.write_text(json.dumps(altered, ensure_ascii=False), encoding="utf-8")
            run(name, error)
        finally:
            target_path.write_bytes(original_bytes)
            manifest_path.write_bytes(baseline)

    try:
        run("baseline-full-seal-zip", archive=args.archive, seal=args.seal)
        target = package / "USER_GUIDE.md"
        original_guide = target.read_bytes()
        try:
            target.unlink()
            run("missing-file", "Package directory file count differs from manifest")
        finally:
            target.write_bytes(original_guide)
        try:
            target.write_bytes(bytes([original_guide[0] ^ 1]) + original_guide[1:])
            run("same-length-tamper", "Package file identity mismatch")
        finally:
            target.write_bytes(original_guide)
        extra = package / "unexpected.txt"
        try:
            extra.write_text("extra", encoding="utf-8")
            run("extra-file", "Package directory file count differs from manifest")
        finally:
            extra.unlink()
        extra_directory = package / "unexpected-empty"
        try:
            extra_directory.mkdir()
            run("extra-empty-directory", "Unexpected or reparse package directory")
        finally:
            extra_directory.rmdir()
        changed_manifest("duplicate-file", lambda value: value["files"].__setitem__(1, value["files"][0]),
                         "Invalid or duplicate package inventory path")
        for name, path in [("traversal", "../outside.txt"), ("ads", "file.txt:stream"), ("reserved-name", "aux.txt")]:
            changed_manifest(name, lambda value, path=path: value["files"][0].__setitem__("path", path),
                             "Invalid or duplicate package inventory path")
        for name, size in [("string-size", "1"), ("fraction-size", 1.5), ("boolean-size", True), ("negative-size", -1)]:
            changed_manifest(name, lambda value, size=size: value["files"][0].__setitem__("size", size),
                             "package entry size must be an unsigned JSON integer")
        changed_manifest("wrong-profile", lambda value: value["buildIdentity"]["unifiedProfile"].__setitem__("layoutVersion", 7),
                         "incompatible Unified SC6-V3")
        try:
            manifest_path.write_bytes(baseline.replace(b'{', b'{"schema":"duplicate",', 1))
            run("duplicate-json-key", "Duplicate or case-ambiguous JSON property")
        finally:
            manifest_path.write_bytes(baseline)
        # A junction is only to an owned empty directory, never an external/user path.
        junction_target = root / "junction-target"
        junction_target.mkdir()
        junction = package / "junction"
        junction_script = root / "make-owned-junction.ps1"
        junction_script.write_text("param($Link, $Target)\n$ErrorActionPreference='Stop'\nNew-Item -ItemType Junction -Path $Link -Target $Target | Out-Null\n", encoding="utf-8")
        create_junction = subprocess.run([args.powershell, "-NoProfile", "-File", str(junction_script), str(junction), str(junction_target)],
            capture_output=True, text=True, timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        if create_junction.returncode != 0:
            raise AssertionError("Could not create owned junction fixture: " + create_junction.stderr)
        try:
            run("directory-junction", "Unexpected or reparse package directory")
        finally:
            junction.rmdir()  # Remove only the link itself; never recurse into its target.
        for case in ("zip-symbolic-link", "zip-duplicate", "zip-traversal"):
            case_root = root / case
            case_root.mkdir()
            derived = case_root / args.archive.name
            with zipfile.ZipFile(args.archive) as source, zipfile.ZipFile(derived, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as destination:
                entries = source.infolist()
                for index, original_entry in enumerate(entries):
                    data = source.read(original_entry)
                    entry = copy.copy(original_entry)
                    if index == 0 and case == "zip-symbolic-link":
                        entry.create_system = 3
                        entry.external_attr = (0o120777 << 16)
                    if index == 0 and case == "zip-traversal":
                        entry.filename = "../outside.txt"
                    destination.writestr(entry, data)
                if case == "zip-duplicate":
                    destination.writestr(copy.copy(entries[0]), source.read(entries[0]))
            # Rebind only the outer archive checksum so ZIP entry checks are reached.
            derived_seal = json.loads(args.seal.read_bytes())
            derived_seal["archive"]["size"] = derived.stat().st_size
            derived_seal["archive"]["sha256"] = digest(derived)
            seal = root / (case + ".seal.json")
            seal.write_text(json.dumps(derived_seal), encoding="utf-8")
            run(case, "declares a reparse point or symbolic link" if case == "zip-symbolic-link" else
                "invalid, duplicate, or unmanifested entry", archive=derived, seal=seal)
        run("restored-copy-full-seal-zip", archive=args.archive, seal=args.seal)
        if product_release:
            changed_product_file("product-license-source-binding", "LICENSE", lambda data: data + b"\nchanged\n",
                                 "Product license is not bound")
            def wrong_license(data):
                document = json.loads(data)
                document["packages"][0]["licenseDeclared"] = "Apache-2.0"
                return json.dumps(document).encode()
            changed_product_file("product-spdx-license-binding", "SBOM.spdx.json", wrong_license,
                                 "project license does not match")
            def wrong_source(data):
                document = json.loads(data)
                document["version"] = "6.10.0"
                return json.dumps(document).encode()
            changed_product_file("product-qt-source-binding", "sources/qt-source-manifest.json", wrong_source,
                                 "Qt corresponding source binding is invalid")
            changed_product_file("product-runtime-required", "Qt6Gui.dll", lambda data: None,
                                 "Required product runtime missing")
    finally:
        unchanged = original_inventory == {str(path.relative_to(original)): digest(path) for path in original.rglob("*") if path.is_file()}
        unchanged = unchanged and digest(args.archive) == archive_hash and digest(args.seal) == seal_hash
        summary = {"schema": "PixelBridge.UnifiedPackageNegativeTests.1", "tests": results,
                   "passed": len(results) == (24 if product_release else 20) and all(item["passed"] for item in results), "originalArtifactsUnchanged": unchanged,
                   "packagedExecutablesRun": False, "packageDirectory": str(original), "derivedDirectory": str(package)}
        (root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        if not unchanged:
            raise AssertionError("Original artifact inventory changed")


if __name__ == "__main__":
    main()
