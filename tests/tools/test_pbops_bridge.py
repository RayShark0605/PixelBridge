"""PBRemoteOpsBridge protocol, safety and regression tests.

Runs the real listener as a subprocess against a temporary local root that
stands in for the SMB share (the listener takes any root path by design).
Windows-only; not wired into CTest. Run:

    python -X utf8 tests/tools/test_pbops_bridge.py [--keep] [--only NAME]

Covers: command round-trip, malformed/unknown/duplicate/oversized rejection,
crash recovery, ZIP attack surface, bounded runner behavior, collect budget,
screenshot, display enumeration, start/stop chains, cleanup and shutdown.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import zipfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
BRIDGE = REPOSITORY / "tools" / "PBRemoteOpsBridge"

_spec = importlib.util.spec_from_file_location("pbops", BRIDGE / "local" / "pbops.py")
pbops = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pbops)

KEEP = False
SUITE_ROOT = None
SUITE_WORKSPACE = None
SUITE_LISTENER = None


def start_listener(root: Path, workspace: Path, poll=0.3):
    return subprocess.Popen(
        [sys.executable, str(BRIDGE / "listener" / "pbops_listener.py"),
         "--share-root", str(root), "--workspace", str(workspace),
         "--poll-seconds", str(poll), "--heartbeat-seconds", "1.0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop_listener(process, timeout=15):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(10)


def make_temp_file(suffix, data=b""):
    """Reserve a unique temp path without leaving an empty file behind
    (zipfile's "x" mode refuses to open an existing empty file)."""
    fd, name = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    path = Path(name)
    path.unlink()
    if data:
        path.write_bytes(data)
    return path


def drop_file(root: Path, name: str, source: Path):
    """Stage a file into files/ the same way pbops.py drop does."""
    files_dir = root / "files"
    files_dir.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    shutil.copyfile(source, files_dir / name)
    pbops.atomic_write_bytes(files_dir / (name + ".sha256"),
                             ("{}  {}\n".format(digest, name)).encode("ascii"), root / "staging")
    pbops.atomic_write_bytes(files_dir / (name + ".ready"), b"ready\n", root / "staging")
    return digest


def send_await(root: Path, command_type, params=None, timeout=60, envelope_timeout=None):
    command_id = pbops.command_send(root, command_type, params or {}, envelope_timeout)
    return pbops.command_await(root, command_id, timeout)


def write_raw_command(root: Path, document_bytes):
    """Write a command file directly (for malformed/oversized fixtures)."""
    stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime())
    name = "cmd-{}Z-{}.json".format(stamp, uuid.uuid4().hex[:6])
    pbops.atomic_write_bytes(root / "inbox" / name, document_bytes, root / "staging")
    return name[:-5]


def make_zip(path: Path, entries):
    """entries: list of (arcname, data, external_attr, flag_bits)."""
    with zipfile.ZipFile(path, "x") as archive:
        for arcname, data, external_attr, flag_bits in entries:
            info = zipfile.ZipInfo(arcname, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = external_attr
            info.flag_bits = flag_bits
            archive.writestr(info, data)


def set_zip_general_purpose_bit(path: Path, bit: int):
    """writestr recomputes flag_bits, so patch the stored headers afterwards."""
    data = bytearray(path.read_bytes())
    for magic, offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        position = 0
        while True:
            position = data.find(magic, position)
            if position < 0:
                break
            data[position + offset] |= bit
            position += 4
    path.write_bytes(bytes(data))


PLAIN = 0o644 << 16


class BridgeTests(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.root = SUITE_ROOT
        cls.workspace = SUITE_WORKSPACE

    def await_ok(self, command_type, params=None, timeout=60, status="ok"):
        result = send_await(self.root, command_type, params, timeout)
        self.assertEqual(result.get("status"), status,
                         "unexpected status for {}: {}".format(command_type, result.get("error")))
        return result

    # ---- protocol ----------------------------------------------------------

    def test_01_ping_roundtrip(self):
        result = self.await_ok("ping")
        payload = result["payload"]
        self.assertEqual(payload["listenerVersion"], "1.0.0")
        self.assertIn("hostname", payload)
        self.assertEqual(result["stdout"], "")

    def test_02_malformed_json_rejected(self):
        command_id = write_raw_command(self.root, b"{ this is not json")
        result = pbops.command_await(self.root, command_id, 30)
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["error"]["code"], "invalid-json")

    def test_03_unknown_type_rejected(self):
        command_id = pbops.command_send(self.root, "ping", {})
        # Overwrite the type after sending by writing a second command directly.
        envelope = {"schemaVersion": 1, "id": "x", "type": "format-c", "params": {},
                    "issuedAtLocal": "now"}
        stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime())
        name = "cmd-{}Z-{}.json".format(stamp, uuid.uuid4().hex[:6])
        envelope["id"] = name[:-5]
        pbops.atomic_write_bytes(self.root / "inbox" / name,
                                 json.dumps(envelope).encode(), self.root / "staging")
        result = pbops.command_await(self.root, name[:-5], 30)
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["error"]["code"], "unknown-type")
        pbops.command_await(self.root, command_id, 30)  # drain the ping

    def test_04_unknown_schema_version_rejected(self):
        command_id = write_raw_command(self.root, json.dumps(
            {"schemaVersion": 99, "id": "placeholder", "type": "ping", "params": {}}).encode())
        # id mismatch would also be rejected; either way it must be rejected, not ok
        result = pbops.command_await(self.root, command_id, 30)
        self.assertEqual(result["status"], "rejected")

    def test_05_unknown_envelope_key_rejected(self):
        stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime())
        name = "cmd-{}Z-{}.json".format(stamp, uuid.uuid4().hex[:6])
        envelope = {"schemaVersion": 1, "id": name[:-5], "type": "ping", "params": {},
                    "typo": True}
        pbops.atomic_write_bytes(self.root / "inbox" / name, json.dumps(envelope).encode(),
                                 self.root / "staging")
        result = pbops.command_await(self.root, name[:-5], 30)
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["error"]["code"], "invalid-envelope")

    def test_06_duplicate_id_rejected(self):
        command_id = pbops.command_send(self.root, "ping", {})
        pbops.command_await(self.root, command_id, 30)
        # Replay an identical command file with the same id.
        original = json.loads((self.root / "processed" / (command_id + ".json")).read_text())
        pbops.atomic_write_bytes(self.root / "inbox" / (command_id + ".json"),
                                 json.dumps(original).encode(), self.root / "staging")
        deadline = time.monotonic() + 30
        dup_files = []
        while time.monotonic() < deadline:
            dup_files = sorted((self.root / "results").glob("res-" + command_id + ".dup-*.json"))
            if dup_files:
                break
            time.sleep(0.3)
        self.assertTrue(dup_files, "duplicate result never appeared")
        duplicate = json.loads(dup_files[0].read_text())
        self.assertEqual(duplicate["status"], "rejected")
        self.assertEqual(duplicate["error"]["code"], "duplicate-command-id")

    def test_07_oversized_command_rejected(self):
        command_id = write_raw_command(self.root, b"x" * (70 * 1024))
        result = pbops.command_await(self.root, command_id, 30)
        self.assertEqual(result["status"], "rejected")

    # ---- deploy safety -----------------------------------------------------

    def test_10_deploy_raw_file(self):
        payload = make_temp_file(".bin", os.urandom(4096))
        try:
            digest = drop_file(self.root, "raw-payload.bin", payload)
            result = self.await_ok("deploy", {"runId": "t10-raw", "file": "raw-payload.bin",
                                              "sha256": digest})
            self.assertEqual(result["payload"]["kind"], "file")
            deployed = self.workspace / "runs" / "t10-raw" / "input" / "raw-payload.bin"
            self.assertEqual(hashlib.sha256(deployed.read_bytes()).hexdigest(), digest)
        finally:
            payload.unlink()

    def test_11_deploy_hash_mismatch_rejected(self):
        payload = make_temp_file(".bin", b"mismatch fixture")
        try:
            drop_file(self.root, "mismatch.bin", payload)
            result = send_await(self.root, "deploy",
                                {"runId": "t11-raw", "file": "mismatch.bin",
                                 "sha256": "0" * 64})
            self.assertEqual(result["status"], "rejected")
            self.assertEqual(result["error"]["code"], "hash-mismatch")
        finally:
            payload.unlink()

    def test_12_deploy_valid_zip(self):
        zip_path = make_temp_file(".zip")
        contents = {"Encoder/app.exe": b"synthetic-a", "profile.json": b"synthetic-b",
                    "dir/inner.txt": b"synthetic-c"}
        make_zip(zip_path, [(name, data, PLAIN, 0) for name, data in contents.items()])
        try:
            digest = drop_file(self.root, "valid.zip", zip_path)
            result = self.await_ok("deploy", {"runId": "t12-zip", "file": "valid.zip",
                                              "sha256": digest})
            self.assertEqual(result["payload"]["entryCount"], 3)
            by_name = {item["name"]: item for item in result["payload"]["inventory"]}
            self.assertEqual(set(by_name), set(contents))
            for name, data in contents.items():
                on_disk = self.workspace / "runs" / "t12-zip" / "package" / name
                self.assertEqual(on_disk.read_bytes(), data)
                self.assertEqual(by_name[name]["sha256"], hashlib.sha256(data).hexdigest())
        finally:
            zip_path.unlink()

    def assert_zip_rejected(self, run_id, name, entries, expected_code):
        zip_path = make_temp_file(".zip")
        make_zip(zip_path, entries)
        try:
            digest = drop_file(self.root, name, zip_path)
            result = send_await(self.root, "deploy", {"runId": run_id, "file": name,
                                                      "sha256": digest})
            self.assertEqual(result["status"], "rejected")
            self.assertEqual(result["error"]["code"], expected_code, result["error"]["message"])
        finally:
            zip_path.unlink()

    def test_13_zip_traversal_rejected(self):
        self.assert_zip_rejected("t13", "traversal.zip",
                                 [("../evil.txt", b"x", PLAIN, 0)], "invalid-path")

    def test_14_zip_symlink_rejected(self):
        self.assert_zip_rejected("t14", "symlink.zip",
                                 [("link", b"x", (0o120777 << 16), 0)], "zip-symlink")

    def test_15_zip_reparse_rejected(self):
        self.assert_zip_rejected("t15", "reparse.zip",
                                 [("entry", b"x", PLAIN | 0x400, 0)], "zip-reparse")

    def test_16_zip_directory_entry_rejected(self):
        self.assert_zip_rejected("t16", "direntry.zip",
                                 [("folder/", b"", PLAIN, 0)], "zip-directory-entry")

    def test_17_zip_duplicate_entry_rejected(self):
        self.assert_zip_rejected("t17", "dupe.zip",
                                 [("a.txt", b"1", PLAIN, 0), ("a.txt", b"2", PLAIN, 0)],
                                 "zip-duplicate")

    def test_18_zip_encrypted_flag_rejected(self):
        zip_path = make_temp_file(".zip")
        make_zip(zip_path, [("a.txt", b"1", PLAIN, 0)])
        set_zip_general_purpose_bit(zip_path, 1)
        try:
            digest = drop_file(self.root, "encrypted.zip", zip_path)
            result = send_await(self.root, "deploy", {"runId": "t18", "file": "encrypted.zip",
                                                      "sha256": digest})
            self.assertEqual(result["status"], "rejected")
            self.assertEqual(result["error"]["code"], "zip-encrypted")
        finally:
            zip_path.unlink()

    # ---- run-script --------------------------------------------------------

    def make_run_with_script(self, run_id, script_bytes, script_name="probe.ps1"):
        run_dir = self.workspace / "runs" / run_id
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / script_name).write_bytes(script_bytes)
        return run_dir

    def test_20_run_script_ok(self):
        self.make_run_with_script("t20", b'Write-Output "marker-ok"\nExit 0\n')
        result = self.await_ok("run-script", {"runId": "t20", "script": "probe.ps1",
                                              "timeoutSeconds": 60})
        self.assertIn("marker-ok", result["stdout"])
        self.assertEqual(result["exitCode"], 0)

    def test_21_run_script_nonzero_exit(self):
        self.make_run_with_script("t21", b'Write-Error "boom"\nExit 5\n')
        result = self.await_ok("run-script", {"runId": "t21", "script": "probe.ps1",
                                              "timeoutSeconds": 60}, status="error")
        self.assertEqual(result["exitCode"], 5)

    def test_22_run_script_timeout(self):
        self.make_run_with_script("t22", b'Start-Sleep -Seconds 60\n')
        started = time.monotonic()
        result = self.await_ok("run-script", {"runId": "t22", "script": "probe.ps1",
                                              "timeoutSeconds": 3}, status="timeout")
        self.assertLess(time.monotonic() - started, 45)
        self.assertEqual(result["error"]["code"], "script-timeout")

    def test_23_run_script_output_cap(self):
        filler = "$s = 'x' * 4096; " + "Write-Output $s; " * 40 + "\n"
        self.make_run_with_script("t23", filler.encode())
        result = self.await_ok("run-script", {"runId": "t23", "script": "probe.ps1",
                                              "timeoutSeconds": 120})
        self.assertLessEqual(len(result["stdout"].encode()), 65536)
        self.assertTrue(result["payload"]["stdoutTruncated"])

    # ---- start / stop ------------------------------------------------------

    def deploy_cmd_dummy(self, run_id):
        cmd_exe = Path(os.environ.get("ComSpec", r"C:\Windows\System32\cmd.exe"))
        run_dir = self.workspace / "runs" / run_id
        run_dir.mkdir(parents=True, exist_ok=True)
        target = run_dir / "input"
        target.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cmd_exe, target / "dummy.cmd.exe")

    def test_30_stop_refuses_unregistered_pid(self):
        result = send_await(self.root, "stop", {"pid": 4})
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["error"]["code"], "not-registered")

    def test_31_start_console_false_and_forced_stop(self):
        self.deploy_cmd_dummy("t31")
        result = self.await_ok("start", {
            "runId": "t31", "exe": "input/dummy.cmd.exe",
            "args": ["/c", "ping", "-n", "120", "127.0.0.1"],
            "console": False, "waitSeconds": 3})
        pid = result["payload"]["pid"]
        stop = self.await_ok("stop", {"runId": "t31", "graceSeconds": 6})
        outcome = stop["payload"]["targets"][0]
        self.assertTrue(outcome["exited"])
        self.assertTrue(outcome["forced"])  # no window, no console: only terminate remains

    def test_32_start_console_q_graceful_stop(self):
        # `pause` is a cmd built-in (no PATH shadowing) that blocks on real
        # console input, exactly like the Encoder's --manual-stop loop.
        self.deploy_cmd_dummy("t32")
        result = self.await_ok("start", {
            "runId": "t32", "exe": "input/dummy.cmd.exe",
            "args": ["/c", "pause"],
            "console": True, "waitSeconds": 3})
        stop = self.await_ok("stop", {"runId": "t32", "graceSeconds": 12})
        outcome = stop["payload"]["targets"][0]
        self.assertTrue(outcome["exited"])
        self.assertEqual(outcome["method"], "console-q")
        self.assertFalse(outcome["forced"])

    # ---- collect / screenshot / display / list / cleanup -------------------

    def test_40_collect_budget_and_manifest(self):
        zip_path = make_temp_file(".zip")
        make_zip(zip_path, [("small-a.bin", b"a" * 100, PLAIN, 0),
                            ("small-b.bin", b"b" * 100, PLAIN, 0),
                            ("big-c.bin", b"c" * 400, PLAIN, 0)])
        try:
            digest = drop_file(self.root, "budget.zip", zip_path)
            self.await_ok("deploy", {"runId": "t40", "file": "budget.zip", "sha256": digest})
            result = self.await_ok("collect", {"runId": "t40",
                                               "patterns": ["*.bin"],
                                               "maxBytes": 250})
            names = {item["name"] for item in result["payload"]["collected"]}
            skipped = {item["name"] for item in result["payload"]["skipped"]}
            self.assertLessEqual(len(names), 2)
            self.assertTrue(names)
            self.assertIn("package/big-c.bin", skipped)
            artifacts_dir = self.root / "results" / ("res-{}-artifacts".format(result["commandId"]))
            for item in result["payload"]["collected"]:
                self.assertTrue((artifacts_dir / item["name"]).is_file())
                self.assertEqual(hashlib.sha256((artifacts_dir / item["name"]).read_bytes()).hexdigest(),
                                 item["sha256"])
        finally:
            zip_path.unlink()

    def test_41_screenshot(self):
        result = self.await_ok("screenshot", {"monitorIndex": 0}, timeout=90)
        shots = result["payload"]["shots"]
        self.assertGreaterEqual(len(shots), 1)
        artifacts_dir = self.root / "results" / ("res-{}-artifacts".format(result["commandId"]))
        png = artifacts_dir / Path(shots[0]["name"]).name
        self.assertEqual(png.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
        self.assertGreater(shots[0]["sizeBytes"], 1000)

    def test_42_display_info_readonly(self):
        result = self.await_ok("display-info", {})
        devices = result["payload"]["devices"]
        self.assertGreaterEqual(len(devices), 1)
        self.assertIn("currentMode", devices[0])

    def test_43_list_runs(self):
        result = self.await_ok("list-runs")
        run_ids = {run["runId"] for run in result["payload"]["runs"]}
        self.assertIn("t12-zip", run_ids)

    def test_44_cleanup_refuses_live_run_then_deletes(self):
        self.deploy_cmd_dummy("t44")
        self.await_ok("start", {"runId": "t44", "exe": "input/dummy.cmd.exe",
                                "args": ["/c", "ping", "-n", "120", "127.0.0.1"],
                                "console": False, "waitSeconds": 2})
        blocked = self.await_ok("cleanup", {"runs": ["t44"]})
        self.assertTrue(any(item["reason"] == "live-process-registered"
                            for item in blocked["payload"]["skipped"]))
        self.assertTrue((self.workspace / "runs" / "t44").exists())
        self.await_ok("stop", {"runId": "t44", "graceSeconds": 6})
        removed = self.await_ok("cleanup", {"runs": ["t44"]})
        self.assertIn("runs/t44", removed["payload"]["deleted"])
        self.assertFalse((self.workspace / "runs" / "t44").exists())

    # ---- isolated lifecycle: crash recovery --------------------------------

    def test_50_crash_recovery_writes_interrupted(self):
        root = Path(tempfile.mkdtemp(prefix="pbops-t50-root-"))
        workspace = Path(tempfile.mkdtemp(prefix="pbops-t50-ws-"))
        listener = None
        try:
            for name in ("inbox", "processed", "results", "status", "files", "staging"):
                (root / name).mkdir(parents=True)
            listener = start_listener(root, workspace)
            run_dir = workspace / "runs" / "t50"
            run_dir.mkdir(parents=True)
            (run_dir / "sleep.ps1").write_bytes(b"Start-Sleep -Seconds 30\n")
            command_id = pbops.command_send(root, "run-script",
                                            {"runId": "t50", "script": "sleep.ps1",
                                             "timeoutSeconds": 60})
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if (root / "processed" / (command_id + ".json")).exists():
                    break
                time.sleep(0.2)
            self.assertTrue((root / "processed" / (command_id + ".json")).exists(),
                            "command was never picked up")
            listener.kill()
            listener.wait(10)
            listener = None
            # The killed listener's job object also reaps the sleeper child.
            restarted = start_listener(root, workspace)
            try:
                result = pbops.command_await(root, command_id, 30)
                self.assertEqual(result["status"], "interrupted")
                self.assertEqual(result["error"]["code"], "listener-restarted")
                followup = send_await(root, "ping", {})
                self.assertEqual(followup["status"], "ok")
            finally:
                stop_listener(restarted)
        finally:
            if listener is not None:
                stop_listener(listener)
            if not KEEP:
                shutil.rmtree(root, ignore_errors=True)
                shutil.rmtree(workspace, ignore_errors=True)

    def test_51_second_instance_exits(self):
        root = Path(tempfile.mkdtemp(prefix="pbops-t51-root-"))
        workspace = Path(tempfile.mkdtemp(prefix="pbops-t51-ws-"))
        first = second = None
        try:
            for name in ("inbox", "processed", "results", "status", "files", "staging"):
                (root / name).mkdir(parents=True)
            first = start_listener(root, workspace)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not (root / "status" / "heartbeat.json").is_file():
                time.sleep(0.2)
            self.assertTrue((root / "status" / "heartbeat.json").is_file())
            second = start_listener(root, workspace)
            try:
                second.wait(15)
                self.assertEqual(second.returncode, 0)
            except subprocess.TimeoutExpired:
                self.fail("second instance did not exit")
            self.assertIsNone(first.poll())  # first instance still running
        finally:
            stop_listener(first)
            stop_listener(second)
            if not KEEP:
                shutil.rmtree(root, ignore_errors=True)
                shutil.rmtree(workspace, ignore_errors=True)

    # ---- shutdown of the shared listener (runs last, z-prefix) -------------

    def test_zzz_listener_shutdown(self):
        result = send_await(self.root, "listener-shutdown", {"confirm": True})
        self.assertEqual(result["status"], "ok")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and SUITE_LISTENER.poll() is None:
            time.sleep(0.3)
        self.assertIsNotNone(SUITE_LISTENER.poll())


def setUpModule():
    global SUITE_ROOT, SUITE_WORKSPACE, SUITE_LISTENER
    SUITE_ROOT = Path(tempfile.mkdtemp(prefix="pbops-suite-root-"))
    SUITE_WORKSPACE = Path(tempfile.mkdtemp(prefix="pbops-suite-ws-"))
    for name in ("inbox", "processed", "results", "status", "files", "staging"):
        (SUITE_ROOT / name).mkdir(parents=True)
    SUITE_LISTENER = start_listener(SUITE_ROOT, SUITE_WORKSPACE)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if (SUITE_ROOT / "status" / "heartbeat.json").is_file():
            return
        time.sleep(0.2)
    stop_listener(SUITE_LISTENER)
    raise RuntimeError("shared listener never became ready")


def tearDownModule():
    if SUITE_LISTENER is not None:
        stop_listener(SUITE_LISTENER)
    if not KEEP:
        shutil.rmtree(SUITE_ROOT, ignore_errors=True)
        shutil.rmtree(SUITE_WORKSPACE, ignore_errors=True)


def main():
    global KEEP
    parser = argparse.ArgumentParser()
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--only", help="run a single test method")
    args = parser.parse_args()
    KEEP = args.keep
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BridgeTests)
    if args.only:
        suite = unittest.TestSuite([test for test in suite
                                    if test.id().endswith("." + args.only)])
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
