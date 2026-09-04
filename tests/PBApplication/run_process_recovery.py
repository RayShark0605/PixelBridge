"""G18 serial supervisor: create-only CSPRNG fixture, real process death and restart.

No desktop, input, capture, GPU, socket, pipe payload or product CLI is used.
Each child contains the production sender/receiver headless closed loop. Its
total process peak is a conservative receiver upper bound, not a fabricated
measurement of an independently running decoder process.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import time
import traceback

import blake3

MIB = 1024 * 1024
FIXTURE_BYTES = 256 * MIB
POINTS = (
    "encoder-prescan", "lease-persisted", "encoder-mid-segment", "decoder-active",
    "part-flushed", "completed-record", "whole-digest", "before-rename", "after-rename",
)
MINIMUM_COMPLETED = dict(zip(POINTS, (0, 0, 2, 2, 2, 3, 32, 32, 32)))
NEGATIVE_CASES = ("torn-final-tail", "internal-crc", "forged-length", "over-active-quota", "changed-source")
SOURCE_FILES = (
    "apps/common/local_desktop_runtime.cpp", "apps/common/decoder_resume_store.cpp",
    "apps/common/encoder_session_store.cpp", "libs/PBStorage/src/output_file.cpp",
    "tests/PBApplication/CMakeLists.txt", "tests/PBApplication/process_fault_test_hook.h",
    "tests/PBApplication/process_recovery_test_access.h", "tests/PBApplication/process_recovery_runtime.inc",
    "tests/PBApplication/process_recovery_worker.cpp", "tests/PBApplication/run_process_recovery.py",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def write_json(path: Path, value: object) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def file_digests(path: Path) -> dict:
    sha256 = hashlib.sha256()
    digest = blake3.blake3()
    length = 0
    with path.open("rb") as stream:
        while block := stream.read(MIB):
            sha256.update(block)
            digest.update(block)
            length += len(block)
    return {"bytes": length, "sha256": sha256.hexdigest(), "blake3": digest.hexdigest()}


def create_fixture(root: Path) -> dict:
    source = root / "fixture.bin"
    with source.open("xb") as stream:
        for _ in range(256):
            stream.write(os.urandom(MIB))
        stream.flush()
        os.fsync(stream.fileno())
    fixture = {"schema": "PixelBridge.G18CsprngFixture.1", "sourcePath": str(source),
               "generator": "Python os.urandom / Windows OS CSPRNG, 256 x 1 MiB; RAW sender",
               "digests": file_digests(source)}
    write_json(root / "fixture-manifest.json", fixture)
    return fixture


def run_worker(worker: Path, source: Path, case: Path, label: str, point: str = "", mode: str = "combined",
               expected_exit: int = 0) -> tuple[Path, float]:
    evidence = case / label
    evidence.mkdir()
    for name in ("sender", "output"):
        (case / name).mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment.pop("PB_G18_EVIDENCE_DIRECTORY", None)
    environment["PB_G18_CRASH_POINT"] = point
    command = [str(worker), mode, str(source), str(case / "sender"), str(case / "output"), str(evidence)]
    started = time.monotonic()
    with (evidence / "stdout.txt").open("xb") as stdout, (evidence / "stderr.txt").open("xb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=environment,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            exit_code = process.wait(timeout=600)
        except subprocess.TimeoutExpired:
            # Only this supervisor's still-live, explicitly owned child is killed.
            process.kill()
            process.wait()
            raise RuntimeError(f"G18 worker exceeded 600 s: {case.name}/{label}")
    elapsed = time.monotonic() - started
    write_json(evidence / "process.json", {"command": command, "pid": process.pid, "exitCode": exit_code,
                                         "elapsedSeconds": elapsed, "selectedCrashPoint": point})
    detail = (evidence / "error.txt").read_text(encoding="utf-8") if (evidence / "error.txt").exists() else ""
    require(exit_code == expected_exit, f"{case.name}/{label}: exit {exit_code}, expected {expected_exit}; {detail}")
    if expected_exit == 218:
        marker = read_json(evidence / "crash.json")
        require(marker["point"] == point and marker["pid"] == process.pid, "Termination marker identity mismatch")
    elif expected_exit == 0:
        require(not (evidence / "crash.json").exists(), "Successful worker retained an unexpected termination marker")
        require((evidence / "result.json").is_file(), "Worker exited without a result")
    return evidence, elapsed


def crc32c(data: bytes | memoryview) -> int:
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)
    checksum = 0xFFFFFFFF
    for byte in data:
        checksum = table[(checksum ^ byte) & 255] ^ (checksum >> 8)
    return checksum ^ 0xFFFFFFFF


def inspect_journal(output: Path) -> dict:
    journals = list(output.glob("*.resume"))
    if not journals:
        return {"bytes": 0, "completedOrdinals": [], "activeOrdinals": [], "records": []}
    require(len(journals) == 1 and journals[0].stat().st_size <= 32 * MIB, "Unexpected G18 journal shape or size")
    path = journals[0]
    document = path.read_bytes()
    require(document[:4] == b"PBJH" and len(document) >= 28, "Invalid G18 journal header")
    position = 28 + struct.unpack_from("<I", document, 16)[0]
    require(position <= len(document), "Truncated G18 journal header")
    require(crc32c(memoryview(document)[:position - 4]) == struct.unpack_from("<I", document, position - 4)[0],
            "G18 journal header CRC mismatch")
    records, completed, active = [], set(), set()
    generation = 0
    while position < len(document):
        require(len(document) - position >= 36 and document[position:position + 4] == b"PBJR", "Incomplete baseline journal")
        version, kind, length, payload_length, next_generation, reserved = struct.unpack_from("<HHIIQQ", document, position + 4)
        require(version == 1 and reserved == 0 and length == payload_length + 36 and
                position + length <= len(document) and next_generation > generation, "Invalid baseline journal envelope")
        require(crc32c(memoryview(document)[position:position + length - 4]) ==
                struct.unpack_from("<I", document, position + length - 4)[0], "Baseline journal record CRC mismatch")
        if kind == 4:
            completed.add(struct.unpack_from("<Q", document, position + 32 + 16)[0])
        if kind == 3:
            active.add(struct.unpack_from("<Q", document, position + 32)[0])
        records.append({"offset": position, "bytes": length, "type": kind, "generation": next_generation})
        generation = next_generation
        position += length
    return {"path": str(path), "bytes": len(document), "sha256": hashlib.sha256(document).hexdigest(),
            "completedOrdinals": sorted(completed), "activeOrdinals": sorted(active - completed), "records": records}


def read_frames(evidence: Path) -> list[dict]:
    path = evidence / "frames.jsonl"
    if not path.exists():
        return []
    require(path.stat().st_size <= 32 * MIB, "Unbounded G18 identity trace")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def verify_ids(before: Path, after: Path, point: str) -> dict:
    after_initial = read_json(after / "initial.json")
    if point == "encoder-prescan":
        require(not (before / "initial.json").exists() and not after_initial["resumed"],
                "Incomplete prescan was incorrectly adopted as a durable Session")
        return {"prescanHadNoDurableSession": True}
    before_initial = read_json(before / "initial.json")
    require(before_initial["sessionId"] == after_initial["sessionId"] and after_initial["resumed"], "Restart changed a valid Session")
    before_frames, after_frames = read_frames(before), read_frames(after)
    require(before_frames, "Missing pre-termination frame/lease ledger")
    before_sequences = {frame["sequence"] for frame in before_frames}
    after_sequences = {frame["sequence"] for frame in after_frames}
    require(len(before_sequences) == len(before_frames) and len(after_sequences) == len(after_frames), "FrameSequence repeated inside a run")
    require(not before_sequences & after_sequences, "FrameSequence was reused after process death")
    maximum_lease = max(frame["frameLeaseEnd"] for frame in before_frames)
    require(after_initial["frameStart"] >= maximum_lease, "Restart did not skip the durable FrameSequence lease")
    before_repairs, after_repairs = set(), set()
    for frames, repair_ids in ((before_frames, before_repairs), (after_frames, after_repairs)):
        for frame in frames:
            require(frame["sequence"] < frame["frameLeaseEnd"], "A frame escaped its durable lease")
            for block_id in frame["repairIds"]:
                key = (frame["ordinal"], block_id)
                require(key not in repair_ids and block_id < frame["repairLeaseEnd"], "Repair ID repeated or escaped its durable lease")
                repair_ids.add(key)
    require(not before_repairs & after_repairs, "Repair ID reused after process death")
    for frame in before_frames:
        require(after_initial["repairStarts"][frame["ordinal"]] >= frame["repairLeaseEnd"], "Restart did not skip a durable repair lease")
    return {"sameSession": True, "frameIdentityOverlap": 0, "repairIdentityOverlap": 0,
            "beforeFrames": len(before_frames), "afterFrames": len(after_frames),
            "beforeRepairIds": len(before_repairs), "afterRepairIds": len(after_repairs), "skippedFrameLeaseEnd": maximum_lease}


def verify_published(case: Path, source: Path, fixture: dict, evidence: Path) -> dict:
    result = read_json(evidence / "result.json")
    require(result["published"] and result["finalReopenVerified"] and result["verifiedSegments"] == 32 and
            result["verifiedBytes"] == FIXTURE_BYTES, "Authoritative final publication was not complete")
    output = case / "output" / source.name
    actual = file_digests(output)
    require(actual == fixture["digests"], f"External final length/SHA-256/BLAKE3 mismatch: {output}")
    require(not list((case / "output").glob("*.part")) and not list((case / "output").glob("*.resume")),
            "Successful publication left active part/resume artifacts")
    require(result["senderResidentSegments"] <= 2 and result["senderResidentEncodedBytes"] <= 16 * MIB and
            result["receiverActiveDecoders"] <= 4 and result["resumeResidentPayloadBytes"] <= 512 * MIB,
            "Working-set resource contract failed")
    # This stricter fixture check applies to the *whole combined process*, so
    # it is also a conservative bound for its receiver component. It is not a
    # replacement for the four-active product resource limits above.
    require(result["peakWorkingSetBytes"] < FIXTURE_BYTES, "Combined process peak reached a whole-file-sized working set")
    return {"outputPath": str(output), "externalDigests": actual, "worker": result}


def verify_restoration(point: str, journal: dict, evidence: Path) -> dict:
    if point == "after-rename":
        # G05 validates the complete durable intent/manifest and the already
        # renamed final file. It deliberately does not replay completed ranges
        # or active FEC through RestoreResumeState, so there is no restore.json.
        result = read_json(evidence / "result.json")
        require(len(journal["completedOrdinals"]) == 32 and result["restoredVerifiedSegments"] == 32 and
                result["finalReopenVerified"] and result["frameCount"] == 0 and result["transportCount"] == 0,
                "Post-rename recovery did not verify all durable Segments without payload replay")
        return {"kind": "PostRenameFinalReopen", "verifiedSegmentsAtSessionAdmission": 32, "payloadReplayBlocks": 0}
    restore_path = evidence / "restore.json"
    restored = read_json(restore_path) if restore_path.exists() else {}
    require(restored.get("completedSegments", 0) == len(journal["completedOrdinals"]),
            "Durable completed Segment was lost during journal reload")
    if point == "decoder-active":
        require(restored.get("activeBlocks", 0) > 0, "Active FEC was not replayed after process restart")
    return {"kind": "SegmentJournalReplay", **restored}


def copy_state(original: Path, derived: Path) -> None:
    derived.mkdir()
    for name in ("sender", "output"):
        source = original / name
        for path in (source, *source.rglob("*")):
            require(not (path.lstat().st_file_attributes & 0x400), "Refusing to copy a reparse-point fixture")
        shutil.copytree(source, derived / name)


def run_negative_checks(worker: Path, source: Path, root: Path, active_snapshot: Path, fixture: dict,
                        selected: tuple[str, ...] = NEGATIVE_CASES) -> list[dict]:
    results = []
    for mutation, expected_error in (
        ("torn-final-tail", ""),
        ("internal-crc", "resume journal internal record CRC is invalid"),
        ("forged-length", "resume journal record header is invalid or non-monotonic"),
    ):
        if mutation not in selected:
            continue
        case = root / mutation
        copy_state(active_snapshot, case)
        baseline = inspect_journal(case / "output")
        path = Path(baseline["path"])
        document = bytearray(path.read_bytes())
        if mutation == "torn-final-tail":
            document += b"PBJ"
        elif mutation == "internal-crc":
            first = baseline["records"][0]
            document[first["offset"] + first["bytes"] - 1] ^= 1
        else:
            first = baseline["records"][0]
            struct.pack_into("<I", document, first["offset"] + 8, first["bytes"] + 1)
        with path.open("r+b") as stream:
            stream.write(document)
            stream.truncate()
            stream.flush()
            os.fsync(stream.fileno())
        write_json(case / "mutation.json", {"kind": mutation, "originalJournal": baseline,
                                           "derivedSha256": hashlib.sha256(document).hexdigest()})
        evidence, _ = run_worker(worker, source, case, "restart", expected_exit=1 if expected_error else 0)
        if expected_error:
            detail = (evidence / "error.txt").read_text(encoding="utf-8")
            require(expected_error in detail and not (case / "output" / source.name).exists(), "Corrupt resume did not fail closed")
            results.append({"case": mutation, "rejected": True, "reason": detail, "wrongPublication": False})
        else:
            require(read_json(evidence / "restore.json")["truncatedTail"], "Clearly torn final tail was not identified")
            results.append({"case": mutation, "tornTailIgnored": True, **verify_published(case, source, fixture, evidence)})
        write_json(case / "verified.json", results[-1])
        print(f"PASS negative {mutation}", flush=True)

    if "over-active-quota" in selected:
        quota_case = root / "over-active-quota"
        quota_case.mkdir()
        seed_case = quota_case / "baseline"
        seed_case.mkdir()
        run_worker(worker, source, seed_case, "seed", "quota-seed", "quota-seed", 218)
        baseline = inspect_journal(seed_case / "output")
        require(baseline["activeOrdinals"] == [0, 1, 2, 3], "Quota seed did not contain four valid active caches")
        derived_case = quota_case / "derived"
        copy_state(seed_case, derived_case)
        journal_path = Path(inspect_journal(derived_case / "output")["path"])
        document = journal_path.read_bytes()
        original = [record for record in baseline["records"] if record["type"] == 3][-1]
        forged = bytearray(document[original["offset"]:original["offset"] + original["bytes"]])
        ordinal, block_id, declared, reserved, padded = struct.unpack_from("<QIHHI", forged, 32)
        require(ordinal == 3 and block_id == 0 and reserved == 0 and 0 < declared == padded <= MIB and
                len(forged) == padded + 56, "Unexpected accepted-block fixture format")
        next_generation = baseline["records"][-1]["generation"] + 1
        require(next_generation < 1 << 64, "Quota fixture generation overflow")
        struct.pack_into("<Q", forged, 16, next_generation)
        struct.pack_into("<Q", forged, 32, 4)
        with source.open("rb") as stream:
            stream.seek(4 * 8 * MIB)
            payload = stream.read(padded)
        require(len(payload) == padded, "Quota fixture source block is truncated")
        forged[52:-4] = payload
        struct.pack_into("<I", forged, len(forged) - 4, crc32c(memoryview(forged)[:-4]))
        with journal_path.open("ab") as stream:
            stream.write(forged)
            stream.flush()
            os.fsync(stream.fileno())
        journal = inspect_journal(derived_case / "output")
        require(journal["activeOrdinals"] == [0, 1, 2, 3, 4], "Quota fixture did not contain five active Segment caches")
        write_json(quota_case / "mutation.json", {"kind": "fifth-valid-active-cache", "originalJournal": baseline,
                                                   "derivedJournal": journal, "productionPolicyUnchanged": True})
        evidence, _ = run_worker(worker, source, derived_case, "restart", expected_exit=1)
        detail = (evidence / "error.txt").read_text(encoding="utf-8")
        require("resume journal exceeds the active Segment limit" in detail and
                not (derived_case / "output" / source.name).exists() and not list((derived_case / "output").glob("*.part")),
                "Over-active cache was not rejected before output preallocation")
        results.append({"case": "over-active-quota", "activeCaches": 5, "productionLimit": 4, "rejected": True, "reason": detail})
        write_json(quota_case / "verified.json", results[-1])
        print("PASS negative over-active-quota", flush=True)

    if "changed-source" not in selected:
        return results
    changed_case = root / "changed-source"
    changed_case.mkdir()
    changed_source = changed_case / source.name
    shutil.copyfile(source, changed_source)
    before, _ = run_worker(worker, changed_source, changed_case, "before", "lease-persisted", "sender", 218)
    original_stat = changed_source.stat()
    with changed_source.open("r+b") as stream:
        original_byte = stream.read(1)
        stream.seek(0)
        stream.write(bytes([original_byte[0] ^ 1]))
        stream.flush()
        os.fsync(stream.fileno())
    os.utime(changed_source, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
    current_stat = changed_source.stat()
    require((original_stat.st_ino, original_stat.st_size, original_stat.st_mtime_ns) ==
            (current_stat.st_ino, current_stat.st_size, current_stat.st_mtime_ns), "Source mutation did not preserve identity/length/mtime")
    after, _ = run_worker(worker, changed_source, changed_case, "after", mode="sender")
    old_state, new_state = read_json(before / "initial.json"), read_json(after / "initial.json")
    require(old_state["sessionId"] != new_state["sessionId"] and not new_state["resumed"] and
            old_state["wholeFileBlake3"] != new_state["wholeFileBlake3"], "Changed contents reused the old Session")
    results.append({"case": "changed-source", "identityLengthAndMtimePreserved": True, "oldSessionRejected": True,
                    "beforeSessionId": old_state["sessionId"], "afterSessionId": new_state["sessionId"],
                    "senderOnlyWorker": read_json(after / "result.json")})
    write_json(changed_case / "verified.json", results[-1])
    print("PASS negative changed-source", flush=True)
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--run-directory", required=True, type=Path)
    parser.add_argument("--fixture-manifest", type=Path)
    parser.add_argument("--points", nargs="+", choices=POINTS)
    parser.add_argument("--repeat", type=int, choices=(1, 2), default=1)
    parser.add_argument("--negative-checks", action="store_true")
    parser.add_argument("--negative-baseline", type=Path,
                        help="Run only selected negatives against an existing immutable decoder-active state copy")
    parser.add_argument("--negative-cases", nargs="+", choices=NEGATIVE_CASES, default=list(NEGATIVE_CASES))
    args = parser.parse_args()
    worker, root = args.worker.resolve(strict=True), args.run_directory.resolve()
    require(os.name == "nt", "G18 uses Windows process and storage semantics")
    require(not args.negative_baseline or (args.negative_checks and not args.points),
            "A negative-only baseline requires --negative-checks and cannot accompany --points")
    points = [] if args.negative_baseline else (args.points or list(POINTS))
    require(len(set(points)) == len(points), "Repeated injection point in one campaign")
    require(len(set(args.negative_cases)) == len(args.negative_cases), "Repeated negative check in one campaign")
    require(not args.negative_checks or args.negative_baseline or "decoder-active" in points,
            "Negative checks require a decoder-active baseline")
    root.mkdir(parents=True, exist_ok=False)
    try:
        estimated_bytes = FIXTURE_BYTES * (len(points) * args.repeat + (8 if args.negative_checks else 2))
        require(shutil.disk_usage(root).free >= estimated_bytes, "Insufficient free space for create-only G18 artifacts")
        repository = Path(__file__).resolve().parents[2]
        source_seal = {name: hashlib.sha256((repository / name).read_bytes()).hexdigest() for name in SOURCE_FILES}
        provenance = {"worker": str(worker), "workerDigests": file_digests(worker), "sourceSha256": source_seal,
                      "python": sys.version, "platform": platform.platform(), "blake3Version": blake3.__version__,
                      "arguments": sys.argv, "freeBytesBeforeRun": shutil.disk_usage(root).free}
        write_json(root / "provenance.json", provenance)
        fixture = read_json(args.fixture_manifest) if args.fixture_manifest else create_fixture(root)
        source = Path(fixture["sourcePath"]).resolve(strict=True)
        require(fixture["digests"]["bytes"] == FIXTURE_BYTES and file_digests(source) == fixture["digests"], "CSPRNG fixture digest changed")
        if args.fixture_manifest:
            write_json(root / "fixture-reference.json", {"manifestPath": str(args.fixture_manifest.resolve()), "fixture": fixture})
        print(f"Fixture ready: {source}; SHA-256={fixture['digests']['sha256']}", flush=True)
        cases = []
        active_snapshot = args.negative_baseline.resolve(strict=True) if args.negative_baseline else None
        if active_snapshot:
            baseline = inspect_journal(active_snapshot / "output")
            require(baseline["completedOrdinals"] == [0, 1] and baseline["activeOrdinals"] == [2],
                    "Negative-only input is not a G18 decoder-active state copy")
            write_json(root / "negative-baseline.json", {"statePath": str(active_snapshot), "journal": baseline})
        for repetition in range(args.repeat):
            for point in points:
                case = root / f"r{repetition + 1}-{point}"
                case.mkdir()
                before, before_seconds = run_worker(worker, source, case, "terminated", point, expected_exit=218)
                require((case / "output" / source.name).exists() == (point == "after-rename"), "Unexpected publication at the crash boundary")
                journal = inspect_journal(case / "output")
                require(len(journal["completedOrdinals"]) == MINIMUM_COMPLETED[point], "Unexpected durable completed-Segment boundary")
                write_json(case / "before-restart-journal.json", journal)
                if point == "decoder-active":
                    require(journal["activeOrdinals"] == [2], "Active-FEC crash did not retain a durable active cache")
                    if args.negative_checks and active_snapshot is None:
                        active_snapshot = root / "immutable-active-baseline"
                        copy_state(case, active_snapshot)
                after, restart_seconds = run_worker(worker, source, case, "restarted")
                identity = verify_ids(before, after, point)
                publication = verify_published(case, source, fixture, after)
                restored = verify_restoration(point, journal, after)
                evidence = {"point": point, "repetition": repetition + 1, "casePath": str(case),
                            "terminatedSeconds": before_seconds, "restartToPublishSeconds": restart_seconds,
                            "crash": read_json(before / "crash.json"), "restore": restored,
                            "durableCompletedBeforeRestart": journal["completedOrdinals"], "identity": identity, **publication}
                write_json(case / "verified.json", evidence)
                cases.append(evidence)
                print(f"PASS r{repetition + 1} {point}: {len(journal['completedOrdinals'])} durable; "
                      f"restart={restart_seconds:.3f}s; combinedPeak={publication['worker']['peakWorkingSetBytes'] / MIB:.2f} MiB", flush=True)
        negatives = run_negative_checks(worker, source, root, active_snapshot, fixture,
                                       tuple(args.negative_cases)) if args.negative_checks else []
        require(file_digests(source) == fixture["digests"], "Campaign changed the immutable source fixture")
        require(source_seal == {name: hashlib.sha256((repository / name).read_bytes()).hexdigest() for name in SOURCE_FILES},
                "Tested source changed while the campaign was running")
        report = {"schema": "PixelBridge.G18ProcessRecovery.1", "visualChainCovered": False,
                  "processTermination": "TerminateProcess, exit 218 plus flushed metadata marker; fresh process restart",
                  "memoryEvidence": "Combined sender+receiver process peak is a conservative receiver bound; separate sender-only measurement in changed-source case",
                  "allRequestedCasesPassed": True, "caseCount": len(cases), "cases": cases, "negativeChecks": negatives,
                  "wrongPublishedFiles": 0, "fixture": fixture, "provenance": provenance}
        write_json(root / "report.json", report)
        print(f"G18 campaign PASS: {len(cases)} termination/restart cases, {len(negatives)} negative checks; {root / 'report.json'}", flush=True)
        return 0
    except Exception as error:
        write_json(root / "failure.json", {"error": str(error), "traceback": traceback.format_exc()})
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
