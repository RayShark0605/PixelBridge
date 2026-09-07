"""Unpack a sealed locally generated candidate, then run its read-only/GUI smoke routes.

Child PATH is limited to Windows system directories; developer Qt/vcpkg paths
and plugin overrides are removed. This is not a pristine Windows VM claim.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import zipfile


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-result", type=Path, required=True)
    parser.add_argument("--evidence-directory", type=Path, required=True)
    parser.add_argument("--powershell", required=True)
    args = parser.parse_args()
    original = json.loads(args.package_result.read_bytes())
    archive = Path(original["archivePath"])
    require(sha256(archive) == original["archiveSha256"], "Trusted archive hash mismatch")
    root = args.evidence_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    package = root / Path(original["packageDirectory"]).name
    package.mkdir()
    # The archive hash is pinned to the producer result before any extraction.
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            path = package / entry.filename
            require(path.resolve().is_relative_to(package), "ZIP path escaped clean directory")
        source.extractall(package)
    verifier = Path(__file__).resolve().parents[2] / "tools/PBUnifiedRelease/Test-PBUnifiedPortablePackage.ps1"
    results = []
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join([str(Path(os.environ["WINDIR"]) / "System32"), os.environ["WINDIR"]])
    for name in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORM", "QML2_IMPORT_PATH", "QML_IMPORT_PATH"):
        environment.pop(name, None)
    preview = root / "previews"
    preview.mkdir()
    environment["PB_GUI_SMOKE_EVIDENCE_DIR"] = str(preview)
    expected = json.loads((package / "package-manifest.json").read_bytes())
    commit = expected["buildIdentity"]["headCommit"]

    def run(name, command, child_environment=None):
        process = subprocess.run([str(value) for value in command], cwd=root, env=child_environment,
                                 stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
        (root / (name + ".stdout.log")).write_bytes(process.stdout)
        (root / (name + ".stderr.log")).write_bytes(process.stderr)
        results.append({"name": name, "exitCode": process.returncode, "passed": process.returncode == 0})
        require(process.returncode == 0, name + " failed: " + process.stderr.decode("utf-8", errors="replace")[-1200:])
        return process.stdout

    def verify(name, script):
        output = run(name, [args.powershell, "-NoProfile", "-File", script, "-PackageDirectory", package,
                           "-PackageSealPath", original["sealPath"], "-ArchivePath", archive,
                           "-ExpectedManifestSha256", original["manifestSha256"]])
        value = json.loads(output)
        require(value["verified"] and value["packagedExecutablesRun"] is False, "Verifier did not remain read-only")

    passed = False
    try:
        verify("before-startup", verifier)
        for role in ("Encoder", "Decoder"):
            executable = package / role / ("PixelBridge" + role + ".exe")
            run(role + "-version", [executable, "--version"], environment)
            identity = json.loads(run(role + "-identity", [executable, "--build-identity"], environment))
            require(identity["gitCommit"] == commit and identity["applicationName"] == "PixelBridge" + role, "Packaged identity mismatch")
            profile = run(role + "-profile", [executable, "--unified-profile"], environment).strip()
            require(hashlib.sha256(profile).hexdigest() == expected["buildIdentity"]["unifiedProfile"]["manifestSha256"], "Packaged profile mismatch")
            smoke = run(role + "-gui-smoke", [executable, "--gui-smoke"], environment)
            require(b"PASS" in smoke, "Missing explicit GUI smoke success")
        # Now use the actual packaged standalone verifier after the trusted
        # external verifier has established its sealed identity.
        verify("after-startup-packaged-verifier", package / "Test-PBUnifiedPortablePackage.ps1")
        passed = True
    finally:
        summary = {"schema": "PixelBridge.UnifiedCleanPackageStartup.1", "passed": passed, "gitCommit": commit,
                   "packageDirectory": str(package), "tests": results, "childPath": environment["PATH"],
                   "offscreenOnly": True, "pristineWindowsVmTested": False, "archiveUnchanged": sha256(archive) == original["archiveSha256"]}
        (root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
