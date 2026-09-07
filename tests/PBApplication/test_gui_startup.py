"""Bounded Windows startup/stdio checks; no product GUI or desktop input injection."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(arguments, **kwargs):
    return subprocess.run([str(item) for item in arguments], timeout=12, check=False, **kwargs)


def check_pe(path):
    data = path.read_bytes()
    require(data[:2] == b"MZ", "Missing DOS header")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    require(data[offset:offset + 4] == b"PE\0\0", "Missing PE signature")
    require(struct.unpack_from("<H", data, offset + 4)[0] == 0x8664, "Not Windows x64")
    require(struct.unpack_from("<H", data, offset + 24)[0] == 0x20B, "Not PE32+")
    require(struct.unpack_from("<H", data, offset + 24 + 68)[0] == 2, "Not GUI subsystem")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--evidence-directory", type=Path, required=True)
    parser.add_argument("--powershell", type=Path, required=True)
    args = parser.parse_args()
    require(os.name == "nt", "Windows only")
    root = args.evidence_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    build = args.build_directory.resolve()
    probe = build / "tests/PBApplication/Release/PBGuiConsoleProbe.exe"
    parent = build / "tests/PBApplication/Release/PBConsoleParentProbe.exe"
    applications = [build / f"apps/PixelBridge{role}/Release/PixelBridge{role}.exe" for role in ("Encoder", "Decoder")]
    results = []

    def case(name, action):
        started = time.monotonic()
        try:
            action()
            results.append({"name": name, "passed": True, "elapsedSeconds": time.monotonic() - started})
            print(f"PASS {name}", flush=True)
        except Exception as error:
            results.append({"name": name, "passed": False, "error": str(error)})
            raise

    def piped_probe():
        process = run([probe, "--redirect-probe"], input=b"pipe-marker\n", capture_output=True)
        require(process.returncode == 0, f"Pipe exit {process.returncode}: {process.stderr!r}")
        require(process.stdout.splitlines() == [b"OUT:pipe-marker"], "stdout pipe was replaced")
        require(process.stderr.splitlines() == [b"ERR:pipe-marker"], "stderr pipe was replaced")

    def files_probe():
        source = root / "stdin.txt"
        source.write_bytes(b"file-marker\n")
        with source.open("rb") as input_file, (root / "stdout.txt").open("xb") as output_file, (root / "stderr.txt").open("xb") as error_file:
            process = run([probe, "--redirect-probe"], stdin=input_file, stdout=output_file, stderr=error_file)
        require(process.returncode == 0, f"File exit {process.returncode}")
        require((root / "stdout.txt").read_bytes().splitlines() == [b"OUT:file-marker"], "stdout file was replaced")
        require((root / "stderr.txt").read_bytes().splitlines() == [b"ERR:file-marker"], "stderr file was replaced")

    def nul_probe():
        process = run([probe, "--redirect-probe"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        require(process.returncode == 0 and process.stderr.splitlines() == [b"ERR:"], "NUL or independently redirected stderr was replaced")

    def no_arguments_probe():
        process = run([probe], capture_output=True)
        require(process.returncode == 0 and process.stdout.splitlines() == [b"GUI_NO_ARGUMENTS_CONSOLE=0"], "No-argument startup attached or allocated a console")

    def hidden_console_probe(mixed_redirection=False):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        arguments = [parent, "--hidden-parent", probe]
        prefix = "hidden-console-parent"
        if mixed_redirection:
            arguments = [parent, "--hidden-parent-redirection", probe, root / "mixed-child-stdout.txt"]
            prefix = "mixed-hidden-console-parent"
        process = run(arguments, capture_output=True,
                      creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
        (root / f"{prefix}.stdout.txt").write_bytes(process.stdout)
        (root / f"{prefix}.stderr.txt").write_bytes(process.stderr)
        require(process.returncode == 0 and b"HIDDEN_PARENT_PASS" in process.stdout, f"Hidden console parent exit {process.returncode}: {process.stderr!r}")

    def application_cli(application):
        version = run([application, "--version"], capture_output=True)
        require(version.returncode == 0 and version.stdout.startswith(application.stem.encode() + b" "), "Version stdout/exit lost")
        identity = run([application, "--build-identity"], capture_output=True)
        require(identity.returncode == 0 and json.loads(identity.stdout)["applicationName"] == application.stem, "Build identity stdout/exit lost")
        runtime_command = "--headless-broadcast" if application.stem.endswith("Encoder") else "--headless-receive"
        error = run([application, runtime_command, "--g22-invalid-command"], capture_output=True)
        require(error.returncode == 2 and error.stderr, f"Invalid-command stderr/exit lost: {error.returncode}")
        (root / f"{application.stem}.version.txt").write_bytes(version.stdout)
        (root / f"{application.stem}.identity.json").write_bytes(identity.stdout)
        (root / f"{application.stem}.invalid.stderr.txt").write_bytes(error.stderr)

    def powershell_redirection(application):
        # Interactive PowerShell does not implicitly wait for bare GUI EXEs.
        # A pipeline (or Start-Process -Wait) preserves synchronous CLI use.
        quoted = str(application).replace("'", "''")
        runtime_command = "--headless-broadcast" if application.stem.endswith("Encoder") else "--headless-receive"
        command = f"& '{quoted}' {runtime_command} --g22-invalid-command 2>&1 | Out-String; exit $LASTEXITCODE"
        process = run([args.powershell, "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True)
        require(process.returncode == 2 and process.stdout, f"PowerShell merged redirection/exit lost: {process.returncode}")

    def powershell_file_redirection(application):
        quoted = str(application).replace("'", "''")
        runtime_command = "--headless-broadcast" if application.stem.endswith("Encoder") else "--headless-receive"
        stdout_path = root / f"{application.stem}.powershell.stdout.txt"
        stderr_path = root / f"{application.stem}.powershell.stderr.txt"
        output = str(stdout_path).replace("'", "''")
        error = str(stderr_path).replace("'", "''")
        command = f"& '{quoted}' {runtime_command} --g22-invalid-command 1> '{output}' 2> '{error}' | Out-Null; exit $LASTEXITCODE"
        process = run([args.powershell, "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True)
        require(process.returncode == 2 and stderr_path.stat().st_size > 20, "PowerShell explicit file redirection did not wait/preserve stderr")

    profiles = []

    def native_smoke_rejects_before_ui(application):
        invalid = [[], [r"\\.\DISPLAY_INVALID", r"\\.\DISPLAY_INVALID", "missing", str(root / "must-not-exist"), "0"],
                   [r"\\.\DISPLAY_INVALID", r"\\.\DISPLAY_INVALID", "missing", str(root / "must-not-exist"), "181"],
                   [r"\\.\DISPLAY_INVALID", r"\\.\DISPLAY_INVALID", "missing", str(root / "must-not-exist"), "15x"],
                   [r"\\.\DISPLAY_INVALID", r"\\.\DISPLAY_INVALID", "missing", str(root / "must-not-exist"), "5"]]
        for arguments in invalid:
            process = run([application, "--gui-native-smoke", *arguments], capture_output=True)
            require(process.returncode == 2, "Native smoke accepted malformed or nonexistent monitor authority")
            require(not (root / "must-not-exist").exists(), "Rejected native smoke wrote artifacts")

    def profile_metadata(application):
        process = run([application, "--unified-profile"], capture_output=True)
        require(process.returncode == 0 and not process.stderr, "Compiled profile diagnostic failed")
        profile = json.loads(process.stdout)
        require(profile["schema"] == "PixelBridge.UnifiedProfile.1" and profile["name"] == "PB-Unified-SC6-V3", "Wrong profile schema/name")
        require(profile["visualProfileId"] == 0x5042554E49534333 and profile["layoutVersion"] == 10, "Wrong profile wire identity")
        require((profile["canvasWidth"], profile["canvasHeight"], profile["tileWidth"], profile["tileHeight"]) == (1920, 1080, 6, 6), "Wrong raster geometry")
        require(len(profile["regions"]) == 33 and len(profile["lanes"]) == 3 and len(profile["carriers"]) == 2, "Incomplete compiled manifest")
        require((profile["presentation"]["minimumFps"], profile["presentation"]["defaultFps"], profile["presentation"]["maximumFps"]) == (1, 15, 60), "Wrong cadence contract")
        canonical = process.stdout.strip()
        if profiles:
            require(canonical == profiles[0], "Encoder/Decoder compiled manifests disagree")
        profiles.append(canonical)
        (root / f"{application.stem}.unified-profile.json").write_bytes(canonical)

    try:
        for application in applications:
            case(f"{application.stem}: Windows GUI PE", lambda application=application: check_pe(application))
            case(f"{application.stem}: CLI pipes and errors", lambda application=application: application_cli(application))
            case(f"{application.stem}: PowerShell merged stderr and exit", lambda application=application: powershell_redirection(application))
            case(f"{application.stem}: PowerShell file redirection waits", lambda application=application: powershell_file_redirection(application))
            case(f"{application.stem}: compiled Unified profile", lambda application=application: profile_metadata(application))
            case(f"{application.stem}: native smoke fails closed before UI", lambda application=application: native_smoke_rejects_before_ui(application))
        case("stdio: independently inherited pipes", piped_probe)
        case("stdio: independently inherited files", files_probe)
        case("stdio: NUL and stderr pipe", nul_probe)
        case("GUI branch: no argument, no console", no_arguments_probe)
        case("CLI branch: attach own hidden parent and repair all standard handles", hidden_console_probe)
        case("CLI branch: preserve stdout file while attaching stderr and stdin console", lambda: hidden_console_probe(True))
    finally:
        summary = {
            "schema": "PixelBridge.G22.GuiStartupTests.1", "results": results,
            "allPassed": len(results) == 16 and all(item["passed"] for item in results),
            "applications": [{"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in applications],
            "boundary": {"productGuiOpened": False, "nativeDataWindowOpened": False, "inputAutomation": False,
                         "hiddenOwnedConsoleProbe": True, "manualShellDoubleClick": False},
        }
        with (root / "summary.json").open("x", encoding="utf-8") as output:
            json.dump(summary, output, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
