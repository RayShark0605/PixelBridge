"""PixelBridge remote operations local helper (runs on the controlling PC).

Companion to listener/pbops_listener.py. All paths are configurable:
--root / PBOPS_ROOT override the default <SHARE-DRIVE>:\pbops convenience root.

This tool only moves orchestration data (commands, packages, scripts,
logs, measurement metadata). It must never feed pixels or payload into
the Decoder's capture path (AGENTS.md section 1).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

SCHEMA_VERSION = 1
COMMAND_TYPES = (
    "ping",
    "listener-shutdown",
    "display-info",
    "display-set",
    "display-restore",
    "deploy",
    "start",
    "stop",
    "run-script",
    "collect",
    "screenshot",
    "list-runs",
    "cleanup",
)

HEARTBEAT_FRESH_SECONDS = 10.0


def default_root():
    return Path(os.environ.get("PBOPS_ROOT", "<ShareRoot>"))


def atomic_write_bytes(path: Path, data: bytes, staging_dir: Path):
    staging_dir.mkdir(parents=True, exist_ok=True)
    temp = staging_dir / ("{}.tmp-{}".format(path.name, uuid.uuid4().hex[:8]))
    with temp.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def new_command_id():
    stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime())
    return "cmd-{}Z-{}".format(stamp, uuid.uuid4().hex[:6])


def parse_scalar(raw):
    for converter in (int, float):
        try:
            return converter(raw)
        except ValueError:
            continue
    if raw.lower() in ("true", "false"):
        return raw.lower() == "true"
    return raw


def parse_key_value(raw):
    key, separator, value = raw.partition("=")
    if not separator or not key:
        raise argparse.ArgumentTypeError("expected KEY=VALUE (e.g. --param runId=r20260910a)")
    return key, value


def merge_params(param_pairs, params_json):
    params = {}
    if params_json is not None:
        document = json.loads(params_json)
        if not isinstance(document, dict):
            raise SystemExit("--params-json must be a JSON object")
        params.update(document)
    for key, value in param_pairs:
        if key in params:
            raise SystemExit("Duplicate param: " + key)
        params[key] = parse_scalar(value)
    return params


def command_send(root: Path, command_type, params, timeout_seconds=None):
    inbox = root / "inbox"
    results = root / "results"
    staging = root / "staging"
    if not inbox.is_dir():
        raise SystemExit("Share tree missing under {}; run the init subcommand first".format(root))
    command_id = new_command_id()
    if (results / ("res-{}.json".format(command_id))).exists():
        raise SystemExit("Result already exists for id {}; refusing to reuse".format(command_id))
    envelope = {
        "schemaVersion": SCHEMA_VERSION,
        "id": command_id,
        "type": command_type,
        "params": params,
        "issuedAtLocal": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    if timeout_seconds is not None:
        envelope["timeoutSeconds"] = timeout_seconds
    data = json.dumps(envelope, ensure_ascii=True, indent=2, allow_nan=False).encode("ascii") + b"\n"
    atomic_write_bytes(inbox / (command_id + ".json"), data, staging)
    return command_id


def read_result(root: Path, command_id):
    primary = root / "results" / ("res-{}.json".format(command_id))
    if not primary.is_file():
        return None
    try:
        return json.loads(primary.read_text(encoding="utf-8-sig"))
    except OSError:
        # The share is written by the remote listener and read here over SMB. A
        # reader that arrives while the result is still being flushed observes a
        # sharing violation, which the caller must treat as "not finished yet"
        # rather than as a failed command.
        return None
    except ValueError:
        # Partially flushed JSON: keep polling until the listener replaces the
        # file atomically with the complete result.
        return None


def command_await(root: Path, command_id, timeout_seconds, poll_seconds=0.3):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        result = read_result(root, command_id)
        if result is not None:
            duplicates = sorted((root / "results").glob("res-{}.dup-*.json".format(command_id)))
            if duplicates:
                print("WARNING: duplicate-id results present: {}".format(
                    ", ".join(path.name for path in duplicates)), file=sys.stderr)
            return result
        time.sleep(poll_seconds)
    raise SystemExit("Timed out after {}s waiting for result of {}".format(timeout_seconds, command_id))


def print_result(result):
    summary = {key: result.get(key) for key in ("commandId", "type", "status", "exitCode",
                                                "startedAtRemote", "finishedAtRemote")}
    print(json.dumps(summary, indent=2))
    if result.get("payload"):
        print(json.dumps(result["payload"], indent=2, ensure_ascii=False))
    if result.get("stdout"):
        print("--- stdout ---")
        print(result["stdout"])
    if result.get("stderr"):
        print("--- stderr ---")
        print(result["stderr"])
    if result.get("error"):
        print("--- error ---")
        print(json.dumps(result["error"], indent=2))


def hash_streaming(source: Path, sink=None, chunk_size=1024 * 1024, progress=None):
    digest = hashlib.sha256()
    count = 0
    with source.open("rb") as stream:
        while True:
            chunk = stream.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
            count += len(chunk)
            if sink is not None:
                sink.write(chunk)
            if progress is not None and count % (256 * 1024 * 1024) < chunk_size:
                print("  {} MiB copied...".format(count // (1024 * 1024)), file=sys.stderr)
    return digest.hexdigest(), count


def cmd_init(args):
    root = Path(args.root)
    for name in ("inbox", "processed", "results", "status", "files", "staging"):
        (root / name).mkdir(parents=True, exist_ok=True)
    print("Share tree ready at {}".format(root))
    return 0


def cmd_beat(args):
    root = Path(args.root)
    heartbeat_path = root / "status" / "heartbeat.json"
    if not heartbeat_path.is_file():
        print("NO HEARTBEAT: {} is missing (listener not started or share wrong)".format(heartbeat_path))
        return 2
    age = time.time() - heartbeat_path.stat().st_mtime
    try:
        document = json.loads(heartbeat_path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        print("Heartbeat unreadable: {}".format(error))
        return 2
    document["ageSecondsLocalFs"] = round(age, 1)
    verdict = "FRESH" if age <= HEARTBEAT_FRESH_SECONDS else "STALE"
    document["freshness"] = verdict
    print(json.dumps(document, indent=2))
    return 0 if verdict == "FRESH" else 1


def cmd_send(args):
    root = Path(args.root)
    if args.type not in COMMAND_TYPES:
        raise SystemExit("Unknown command type {}; known: {}".format(args.type, ", ".join(COMMAND_TYPES)))
    params = merge_params(args.param or [], args.params_json)
    command_id = command_send(root, args.type, params, args.timeout)
    print(command_id)
    return 0


def cmd_await(args):
    result = command_await(Path(args.root), args.id, args.timeout)
    print_result(result)
    return 0 if result.get("status") == "ok" else 3


def cmd_run(args):
    root = Path(args.root)
    if args.type not in COMMAND_TYPES:
        raise SystemExit("Unknown command type {}".format(args.type))
    params = merge_params(args.param or [], args.params_json)
    command_id = command_send(root, args.type, params, args.timeout)
    print("sent: {}".format(command_id), file=sys.stderr)
    result = command_await(root, command_id, args.await_timeout)
    print_result(result)
    return 0 if result.get("status") == "ok" else 3


def cmd_drop(args):
    root = Path(args.root)
    source = Path(args.path)
    if not source.is_file():
        raise SystemExit("Not a file: {}".format(source))
    name = args.name or source.name
    if "/" in name or "\\" in name or ":" in name:
        raise SystemExit("name must be a single path component")
    files_dir = root / "files"
    staging = root / "staging"
    target = files_dir / name
    if target.exists() or (files_dir / (name + ".ready")).exists():
        raise SystemExit("files/{} or its .ready marker already exists; cleanup first".format(name))
    files_dir.mkdir(parents=True, exist_ok=True)
    temp = staging / ("drop-{}.tmp-{}".format(name, uuid.uuid4().hex[:8]))
    temp.parent.mkdir(parents=True, exist_ok=True)
    digest = None
    try:
        with temp.open("xb") as sink:
            digest, count = hash_streaming(source, sink=sink, progress=args.progress)
        os.replace(temp, target)
    finally:
        if temp.exists():
            temp.unlink()
    sidecar = files_dir / (name + ".sha256")
    atomic_write_bytes(sidecar, ("{}  {}\n".format(digest, name)).encode("ascii"), staging)
    atomic_write_bytes(files_dir / (name + ".ready"), b"ready\n", staging)
    print(json.dumps({"name": name, "sizeBytes": count, "sha256": digest}))
    return 0


def cmd_pull(args):
    root = Path(args.root)
    command_id = args.id
    result = read_result(root, command_id)
    if result is None:
        raise SystemExit("No result found for {}".format(command_id))
    artifacts_dir = root / "results" / ("res-{}-artifacts".format(command_id))
    if not artifacts_dir.is_dir():
        print("Result has no artifacts directory (nothing to pull).")
        return 0
    destination = Path(args.dest) if args.dest else Path("pbops-out") / command_id
    destination.mkdir(parents=True, exist_ok=True)
    copied = 0
    for path in sorted(artifacts_dir.rglob("*")):
        if not path.is_file():
            continue
        target = destination.joinpath(*path.relative_to(artifacts_dir).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("wb") as sink:
            digest, count = hash_streaming(path, sink=sink)
        copied += 1
        print("{}  {}  {}".format(digest[:16], count, target))
    print("Pulled {} files to {}".format(copied, destination))
    return 0


def start_listener_process(root: Path, workspace: Path, poll_seconds=0.3):
    listener_source = Path(__file__).resolve().parent.parent / "listener" / "pbops_listener.py"
    if not listener_source.is_file():
        raise SystemExit("Listener source not found: {}".format(listener_source))
    process = subprocess.Popen(
        [sys.executable, str(listener_source), "--share-root", str(root),
         "--workspace", str(workspace), "--poll-seconds", str(poll_seconds),
         "--heartbeat-seconds", "1.0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return process


def cmd_selftest(args):
    keep = args.keep
    root = Path(tempfile.mkdtemp(prefix="pbops-selftest-root-"))
    workspace = Path(tempfile.mkdtemp(prefix="pbops-selftest-ws-"))
    process = None
    failures = []

    def step(name, action):
        try:
            action()
            print("  PASS  {}".format(name))
        except Exception as error:  # noqa: BLE001 - selftest reports and continues
            failures.append("{}: {}".format(name, error))
            print("  FAIL  {}: {}".format(name, error))

    def expect(result, status="ok"):
        if result.get("status") != status:
            raise AssertionError("status={} error={}".format(result.get("status"), result.get("error")))

    try:
        for name in ("inbox", "processed", "results", "status", "files", "staging"):
            (root / name).mkdir(parents=True, exist_ok=True)
        process = start_listener_process(root, workspace)

        def wait_heartbeat():
            deadline = time.monotonic() + 15
            path = root / "status" / "heartbeat.json"
            while time.monotonic() < deadline:
                if path.is_file():
                    return
                time.sleep(0.3)
            raise AssertionError("heartbeat never appeared")

        step("listener heartbeat", wait_heartbeat)

        def do_ping():
            command_id = command_send(root, "ping", {})
            expect(command_await(root, command_id, 20))

        step("ping round-trip", do_ping)

        def do_drop_deploy_raw():
            payload = workspace / "payload.bin"
            payload.write_bytes(os.urandom(65536))
            digest = hashlib.sha256(payload.read_bytes()).hexdigest()
            target = root / "files" / "payload.bin"
            shutil.copyfile(payload, target)
            atomic_write_bytes(root / "files" / "payload.bin.sha256",
                               (digest + "  payload.bin\n").encode("ascii"), root / "staging")
            atomic_write_bytes(root / "files" / "payload.bin.ready", b"ready\n", root / "staging")
            command_id = command_send(root, "deploy", {"runId": "selftest-raw", "file": "payload.bin",
                                                       "sha256": digest})
            result = command_await(root, command_id, 30)
            expect(result)
            deployed = workspace / "runs" / "selftest-raw" / "input" / "payload.bin"
            if not deployed.is_file() or hashlib.sha256(deployed.read_bytes()).hexdigest() != digest:
                raise AssertionError("deployed payload hash mismatch")

        step("drop + deploy raw file", do_drop_deploy_raw)

        def do_run_script():
            script_dir = workspace / "runs" / "selftest-raw"
            script_dir.mkdir(parents=True, exist_ok=True)
            (script_dir / "probe.ps1").write_bytes(
                b'Write-Output "selftest-script-ok"\nExit 0\n')
            command_id = command_send(root, "run-script",
                                      {"runId": "selftest-raw", "script": "probe.ps1", "timeoutSeconds": 60})
            result = command_await(root, command_id, 90)
            expect(result)
            if "selftest-script-ok" not in result.get("stdout", ""):
                raise AssertionError("script stdout missing marker")

        step("run-script", do_run_script)

        def do_display_info():
            command_id = command_send(root, "display-info", {})
            expect(command_await(root, command_id, 30))

        step("display-info (read-only)", do_display_info)

        def do_screenshot():
            command_id = command_send(root, "screenshot", {"monitorIndex": 0})
            result = command_await(root, command_id, 60)
            expect(result)
            shots = result.get("payload", {}).get("shots", [])
            if not shots or shots[0].get("sizeBytes", 0) <= 0:
                raise AssertionError("no screenshot artifact")

        step("screenshot", do_screenshot)

        def do_list_runs():
            command_id = command_send(root, "list-runs", {})
            result = command_await(root, command_id, 30)
            expect(result)
            if not any(run.get("runId") == "selftest-raw"
                       for run in result.get("payload", {}).get("runs", [])):
                raise AssertionError("selftest-raw run missing from list-runs")

        step("list-runs", do_list_runs)

        def do_shutdown():
            command_id = command_send(root, "listener-shutdown", {"confirm": True})
            expect(command_await(root, command_id, 20))
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and process.poll() is None:
                time.sleep(0.3)
            if process.poll() is None:
                process.terminate()
                raise AssertionError("listener did not exit after listener-shutdown")

        step("listener-shutdown", do_shutdown)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
        if not keep:
            shutil.rmtree(root, ignore_errors=True)
            shutil.rmtree(workspace, ignore_errors=True)
        else:
            print("kept: root={} workspace={}".format(root, workspace))
    if failures:
        print("SELFTEST FAILED ({} failures)".format(len(failures)))
        return 1
    print("SELFTEST PASSED")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description="PixelBridge remote operations local helper")
    parser.add_argument("--root", default=str(default_root()),
                        help="Bridge share root (default <ShareRoot>; override with PBOPS_ROOT)")
    subparsers = parser.add_subparsers(dest="command", required=True)

    init_parser = subparsers.add_parser("init", help="Create the share tree")
    init_parser.set_defaults(func=cmd_init)

    beat_parser = subparsers.add_parser("beat", aliases=["status"], help="Check listener heartbeat freshness")
    beat_parser.set_defaults(func=cmd_beat)

    send_parser = subparsers.add_parser("send", help="Write one command file; prints its id")
    send_parser.add_argument("type", choices=COMMAND_TYPES)
    send_parser.add_argument("--param", action="append", type=parse_key_value, metavar="KEY=VALUE")
    send_parser.add_argument("--params-json", help="Full params object (JSON)")
    send_parser.add_argument("--timeout", type=int, help="timeoutSeconds for the listener")
    send_parser.set_defaults(func=cmd_send)

    await_parser = subparsers.add_parser("await", help="Wait for one command result")
    await_parser.add_argument("id")
    await_parser.add_argument("--timeout", type=float, default=60.0)
    await_parser.set_defaults(func=cmd_await)

    run_parser = subparsers.add_parser("run", help="send + await in one step")
    run_parser.add_argument("type", choices=COMMAND_TYPES)
    run_parser.add_argument("--param", action="append", type=parse_key_value, metavar="KEY=VALUE")
    run_parser.add_argument("--params-json", help="Full params object (JSON)")
    run_parser.add_argument("--timeout", type=int, help="timeoutSeconds for the listener")
    run_parser.add_argument("--await-timeout", type=float, default=90.0)
    run_parser.set_defaults(func=cmd_run)

    drop_parser = subparsers.add_parser("drop", help="Stage a payload file into files/ with hash + ready marker")
    drop_parser.add_argument("path")
    drop_parser.add_argument("--name", help="Destination file name (default: source name)")
    drop_parser.add_argument("--progress", action="store_true")
    drop_parser.set_defaults(func=cmd_drop)

    pull_parser = subparsers.add_parser("pull", help="Copy result artifacts out of the share")
    pull_parser.add_argument("id")
    pull_parser.add_argument("--dest")
    pull_parser.set_defaults(func=cmd_pull)

    selftest_parser = subparsers.add_parser("selftest", help="End-to-end smoke against a temporary root")
    selftest_parser.add_argument("--keep", action="store_true")
    selftest_parser.set_defaults(func=cmd_selftest)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
