"""Bounded Windows PowerShell 5.1 process fixtures, never a live PixelBridge proof."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


SCRIPTS = Path(__file__).resolve().parent
POWERSHELL = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
CSC = Path(os.environ["SystemRoot"]) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"
COUNTERS = (
    "outerResourceRejections", "outerConflictRejections", "outerDeferredResourceBusyCount",
    "outerFecQuotaExceededCount", "receiverResourcePolicyRejectedCount",
    "receiverControlRejectedByResourcePolicyCount", "outerOrphanDroppedByQuotaCount",
    "outerOrphanResourceExhaustedCount", "outerOrphanConflictRejectionCount",
)
ENVIRONMENT = dict(os.environ, PSModulePath=(
    str(POWERSHELL.parent / "Modules") + ";" + os.environ["ProgramFiles"] + "/WindowsPowerShell/Modules"))


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, indent=2)
        output.write("\n")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stop_owned_fixture(pid, executable):
    """Never terminate an unrelated PID, even after fixture PID reuse."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.TerminateProcess.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x1000 | 0x100000 | 0x0001, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:  # PID no longer exists.
            return False
        raise ctypes.WinError(error)
    try:
        if kernel.WaitForSingleObject(handle, 0) == 0:
            return False
        name = ctypes.create_unicode_buffer(32768)
        count = wintypes.DWORD(len(name))
        if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(count)):
            raise ctypes.WinError(ctypes.get_last_error())
        if Path(name.value).resolve() != executable.resolve():
            raise RuntimeError("Fixture PID ownership changed; refusing cleanup")
        if not kernel.TerminateProcess(handle, 97):
            raise ctypes.WinError(ctypes.get_last_error())
        if kernel.WaitForSingleObject(handle, 5000) != 0:
            raise RuntimeError("Owned fixture cleanup deadline exceeded")
        return True
    finally:
        kernel.CloseHandle(handle)


def run_script(case, script, timeout=12):
    command = [str(POWERSHELL), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)]
    started = time.monotonic()
    timed_out = False
    with (case / "stdout.txt").open("xb") as stdout, (case / "stderr.txt").open("xb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=ENVIRONMENT,
                                   creationflags=subprocess.CREATE_NO_WINDOW, cwd=case)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            process.wait(timeout=5)
        finally:
            pid_path = case / "bin/fixture-pid.txt"
            leaked_child = False
            if pid_path.exists():
                leaked_child = stop_owned_fixture(int(pid_path.read_text()), case / "bin/PBUnifiedRemoteGate.exe")
    return {"command": command, "exitCode": process.returncode, "timedOut": timed_out,
            "outerHarnessKilledChild": leaked_child, "elapsedSeconds": time.monotonic() - started}


def make_fixture(root, fixture, name, wrapper, mode, sender=False):
    case = root / name
    (case / "bin").mkdir(parents=True, exist_ok=False)
    executable = case / "bin" / ("PixelBridgeEncoder.exe" if sender else "PBUnifiedRemoteGate.exe")
    shutil.copyfile(fixture, executable)
    (case / "bin/fixture-mode.txt").write_text(mode, encoding="ascii")
    shutil.copyfile(wrapper, case / wrapper.name)
    write_json(case / "expected-build.json", {"gitCommit": "g21-lifecycle-fixture",
               "encoderSha256": digest(executable), "gateSha256": digest(executable), "blake3Sha256": "0" * 64})
    if sender:
        (case / "Get-G21FileDigests.ps1").write_text("throw 'intentional synthetic digest failure'\n", encoding="ascii")
    return case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--new-run-root", required=True, type=Path)
    parser.add_argument("--baseline-kit-root", type=Path)
    args = parser.parse_args()
    root = args.new_run_root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    fixture = root / "WrapperLifecycleFixture.exe"
    compiler = subprocess.run([str(CSC), "/nologo", "/platform:x64", "/target:exe", "/out:" + str(fixture),
                               str(SCRIPTS / "wrapper_lifecycle_fixture.cs")], capture_output=True, timeout=30,
                              creationflags=subprocess.CREATE_NO_WINDOW)
    (root / "compiler.stdout.txt").write_bytes(compiler.stdout)
    (root / "compiler.stderr.txt").write_bytes(compiler.stderr)
    if compiler.returncode != 0:
        raise RuntimeError("Process fixture compilation failed")
    results = []
    write_json(root / "scope.json", {"fixtureOnly": True, "livePixelTransferTest": False,
        "productBinariesExecuted": False, "sourceBytesReadByReceiver": 0,
        "sourceHashes": {name: digest(SCRIPTS / name) for name in (
            "test_g21_wrappers.py", "wrapper_lifecycle_fixture.cs", "Start-G21Receiver.ps1", "Start-G21Encoder.ps1")}})
    for index, mode in enumerate(("normal", "waiting", "partial", "exit-seven", "coverage", "overflow", *("counter:" + name for name in COUNTERS))):
        # Short artifact components also work with the .NET Framework fixture's MAX_PATH limit.
        case = make_fixture(root, fixture, "r%02d" % index, SCRIPTS / "Start-G21Receiver.ps1", mode)
        result = run_script(case, case / "Start-G21Receiver.ps1")
        runs = list((case / "runs").iterdir())
        assert len(runs) == 1
        report = read_json(runs[0] / "process-exit.json")
        failure = mode in ("coverage", "overflow") or mode.startswith("counter:")
        success = mode in ("normal", "waiting", "partial")
        assert not result["timedOut"] and not result["outerHarnessKilledChild"], result
        assert (result["exitCode"] == 0) == success, (mode, result)
        assert report["forcedTermination"] == failure, (mode, report)
        if failure:
            assert report["failureReason"] and (runs[0] / "receiver-gate-failure-snapshot.json").exists()
        else:
            assert report["receiverExit"] == (7 if mode == "exit-seven" else 0)
        samples = (runs[0] / "memory-samples.jsonl").read_text().splitlines()
        assert 1 <= len(samples) <= 631 and len(samples) == report["memorySamples"]
        launch = read_json(runs[0] / "launch.json")
        assert len(launch["arguments"]) == 3 and launch["arguments"][0] == "--receive"
        assert launch["sourceOrOracleProvided"] is False
        result.update(mode=mode, passed=True, report=report, fixtureOnly=True)
        write_json(case / "result.json", result)
        results.append(result)
        print("PASS synthetic receiver", mode, flush=True)
    for label, wrapper in [("current", SCRIPTS / "Start-G21Encoder.ps1")] + (
            [("baseline", args.baseline_kit_root / "remote-encoder/Start-G21Encoder.ps1")] if args.baseline_kit_root else []):
        case = make_fixture(root, fixture, "sender-" + label, wrapper, "normal", sender=True)
        result = run_script(case, case / "Start-G21Encoder.ps1")
        runs = list((case / "runs").iterdir())
        assert len(runs) == 1 and result["exitCode"] != 0 and not result["timedOut"]
        assert "intentional synthetic digest failure" in (case / "stderr.txt").read_text(errors="replace")
        exit_path = runs[0] / "process-exit.json"
        assert exit_path.exists() == (label == "current")
        if label == "current":
            assert read_json(exit_path)["encoderExit"] == 0
        assert read_json(case / "bin/fixture-source-lease.json")["writeDenied"] is True
        result.update(mode="sender-digest-failure-" + label, passed=True, fixtureOnly=True)
        write_json(case / "result.json", result)
        results.append(result)
        print("PASS synthetic sender digest failure", label, flush=True)
    if args.baseline_kit_root:
        wrapper = args.baseline_kit_root / "local-receiver/Start-G21Receiver.ps1"
        case = make_fixture(root, fixture, "baseline-coverage", wrapper, "coverage")
        result = run_script(case, case / wrapper.name, timeout=6)
        assert result["timedOut"] and result["outerHarnessKilledChild"], result
        result.update(mode="baseline-coverage-reproduced", passed=True, fixtureOnly=True)
        write_json(case / "result.json", result)
        results.append(result)
        print("PASS reproduced baseline missing coverage fail-fast", flush=True)
    for label, wrapper in [("current", SCRIPTS / "Start-G21Receiver.ps1")] + (
            [("baseline", args.baseline_kit_root / "local-receiver/Start-G21Receiver.ps1")] if args.baseline_kit_root else []):
        case = root / ("unstarted-" + label)
        case.mkdir()
        # Exercise the actual wrapper's finally AST without starting any process.
        source = r"""
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile('__WRAPPER__', [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Wrapper parse failed.' }
$blocks = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.TryStatementAst] -and $null -ne $node.Finally }, $true))
$text = $blocks[-1].Finally.Extent.Text
$body = [ScriptBlock]::Create($text.Substring(1, $text.Length - 2))
$run = $PSScriptRoot; $utf8 = [Text.UTF8Encoding]::new($false)
$process = New-Object Diagnostics.Process
$started = $false; $forced = $false; $memoryWriter = $null
$failureReason = $null; $wrapperFailure = 'start sentinel'; $samples = 0; $peakWorkingSet = 0; $peakPrivate = 0
try
{
    try { throw 'start sentinel' } finally { . $body }
}
catch
{
    $preserved = $_.ToString().Contains('start sentinel')
    [IO.File]::WriteAllText((Join-Path $run 'cleanup-result.json'), (@{ fixtureOnly = $true; originalFailurePreserved = $preserved; message = $_.ToString() } | ConvertTo-Json), $utf8)
}
""".replace("__WRAPPER__", str(wrapper).replace("'", "''"))
        script = case / "exercise-finally.ps1"
        script.write_text(source, encoding="utf-8")
        result = run_script(case, script)
        observed = read_json(case / "cleanup-result.json")
        assert result["exitCode"] == 0 and not result["timedOut"]
        # PS 5.1 already preserves this exception in the old wrapper; the new
        # guarantee is an explicit nullable exit record, not a claimed old crash.
        assert observed["originalFailurePreserved"] is True
        assert (case / "process-exit.json").exists() == (label == "current")
        if label == "current":
            report = read_json(case / "process-exit.json")
            assert report["started"] is False and report["receiverExit"] is None
        result.update(mode="unstarted-cleanup-" + label, passed=True, fixtureOnly=True, observed=observed)
        write_json(case / "result.json", result)
        results.append(result)
        print("PASS synthetic unstarted process cleanup", label, flush=True)
    write_json(root / "summary.json", {"passed": True, "fixtureOnly": True, "cases": len(results), "results": results})


if __name__ == "__main__":
    main()
