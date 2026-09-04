"""G19: one create-only 20 GiB+ structured sparse transfer and process restart.

Only the opt-in headless worker runs. No pixel, input, GUI, GPU, ASan or CTest
gate is invoked. Metadata may cross the supervisor boundary; payload travels
through the production Control/Transport/FEC/Receiver/Storage inside the child.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import msvcrt
from pathlib import Path
import shutil
import stat
import struct
import sys
import time
import traceback

import blake3
import run_process_recovery as recovery

GIB = 1024 ** 3
SEGMENT_BYTES = 8 * 1024 ** 2
FILE_BYTES = 20 * GIB + 65536
SEGMENT_COUNT = 2561
CHECKPOINT_ORDINAL = 1024
SEED = bytes.fromhex("719ab40d3e68125cf94d072be138c66a918de74fbaa638529671d5fe1024ac83")
TRANSLATE_NONZERO = bytes(value % 255 + 1 for value in range(256))
SOURCE_FILES = (*recovery.SOURCE_FILES, "tests/PBApplication/run_large_file_recovery.py")


def regions(ordinal: int) -> list[tuple[int, bytes]]:
    """64 KiB nonzero-derived data per Segment, plus explicit head/tail markers."""
    size = min(SEGMENT_BYTES, FILE_BYTES - ordinal * SEGMENT_BYTES)
    spans = ((0, size),) if size < 131072 else ((0, 16384), (size // 2, 16384), (size - 32768, 32768))
    header = struct.pack("<8sQQQ32s", b"PBG19V1\0", ordinal, ordinal * SEGMENT_BYTES, size, SEED)
    output = []
    for offset, length in spans:
        content = bytearray(hashlib.shake_256(SEED + struct.pack("<QQQ", ordinal, offset, size)).digest(length).translate(TRANSLATE_NONZERO))
        if offset == 0:
            content[:64] = header
        if offset + length == size:
            content[-64:] = hashlib.sha256(header).digest() * 2
        output.append((offset, bytes(content)))
    return output


def create_fixture(root: Path) -> dict:
    recovery.require(len(SEED) == 32 and (FILE_BYTES + SEGMENT_BYTES - 1) // SEGMENT_BYTES == SEGMENT_COUNT,
                     "Structured fixture seed or dimensions are invalid")
    source = root / "structured-20gib-plus-64k.bin"
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.DeviceIoControl.argtypes = (wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
    kernel.DeviceIoControl.restype = wintypes.BOOL
    kernel.SetFilePointerEx.argtypes = (wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD)
    kernel.SetFilePointerEx.restype = wintypes.BOOL
    kernel.SetEndOfFile.argtypes = (wintypes.HANDLE,)
    kernel.SetEndOfFile.restype = wintypes.BOOL
    started = time.monotonic()
    with source.open("x+b") as stream:
        enabled, returned = ctypes.c_ubyte(1), wintypes.DWORD()
        # FSCTL_SET_SPARSE: local Windows SDK winioctl.h, no privilege elevation.
        ok = kernel.DeviceIoControl(msvcrt.get_osfhandle(stream.fileno()), 0x000900C4,
                                    ctypes.byref(enabled), 1, None, 0, ctypes.byref(returned), None)
        recovery.require(ok, f"FSCTL_SET_SPARSE failed: {ctypes.get_last_error()}")
        # Windows Python truncate materialized the holes after flush/close in
        # the controlled fixture probe. Set EOF directly; write only our spans.
        handle = msvcrt.get_osfhandle(stream.fileno())
        recovery.require(kernel.SetFilePointerEx(handle, FILE_BYTES, None, 0) and kernel.SetEndOfFile(handle),
                         f"Sparse logical extension failed: {ctypes.get_last_error()}")
        for ordinal in range(SEGMENT_COUNT):
            for relative, content in regions(ordinal):
                stream.seek(ordinal * SEGMENT_BYTES + relative)
                recovery.require(stream.write(content) == len(content), "Short sparse fixture write")
        stream.flush()
        import os
        os.fsync(stream.fileno())
    attributes = source.stat()
    recovery.require(attributes.st_size == FILE_BYTES and attributes.st_file_attributes & stat.FILE_ATTRIBUTE_SPARSE_FILE,
                     "Source is not an actual sparse 20 GiB+ file")
    kernel.GetCompressedFileSizeW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
    kernel.GetCompressedFileSizeW.restype = wintypes.DWORD
    high = wintypes.DWORD()
    ctypes.set_last_error(0)
    low = kernel.GetCompressedFileSizeW(str(source), ctypes.byref(high))
    recovery.require(low != 0xFFFFFFFF or ctypes.get_last_error() == 0, "Source allocation query failed")
    allocated = (high.value << 32) | low
    recovery.require(0 < allocated < GIB, "Structured source unexpectedly allocated a whole large file")
    manifest = {"schema": "PixelBridge.G19StructuredSparseFixture.1", "sourcePath": str(source),
                "seedHex": SEED.hex(), "length": FILE_BYTES, "segmentBytes": SEGMENT_BYTES,
                "segmentCount": SEGMENT_COUNT, "actualAllocatedBytes": allocated, "sparse": True,
                "generator": "SHAKE256(seed || LE64 ordinal || LE64 relativeOffset || LE64 rawSize), byte=(x%255)+1",
                "regions": "full 64 KiB tail; otherwise 16 KiB at start, 16 KiB at midpoint, 32 KiB at end; all holes zero",
                "markers": "64-byte <8sQQQ32s> header PBG19V1\\0, ordinal, raw offset, raw size, seed; repeated SHA256(header) footer",
                "digests": recovery.file_digests(source), "generationAndSealSeconds": time.monotonic() - started}
    recovery.write_json(root / "fixture-manifest.json", manifest)
    return manifest


def verify_all_bytes(source: Path, output: Path, fixture: dict) -> dict:
    recovery.require(source.stat().st_size == output.stat().st_size == FILE_BYTES, "Final logical length mismatch")
    sha256, digest = hashlib.sha256(), blake3.blake3()
    verified_bytes = 0
    started = time.monotonic()
    with source.open("rb") as original, output.open("rb") as published:
        for ordinal in range(SEGMENT_COUNT):
            size = min(SEGMENT_BYTES, FILE_BYTES - ordinal * SEGMENT_BYTES)
            expected = bytearray(size)
            for relative, content in regions(ordinal):
                expected[relative:relative + len(content)] = content
            before, after = original.read(size), published.read(size)
            recovery.require(len(before) == size and before == after == expected,
                             f"Source/output/generator byte comparison failed at Segment {ordinal}")
            sha256.update(after)
            digest.update(after)
            verified_bytes += size
        recovery.require(not original.read(1) and not published.read(1), "Unexpected trailing file bytes")
    actual = {"bytes": verified_bytes, "sha256": sha256.hexdigest(), "blake3": digest.hexdigest()}
    recovery.require(actual == fixture["digests"], "Final external hashes differ from the sealed source")
    return {"segmentsCompared": SEGMENT_COUNT, "allSourceOutputGeneratorBytesEqual": True,
            "externalDigests": actual, "verificationSeconds": time.monotonic() - started}


def read_progress(path: Path) -> list[dict]:
    recovery.require(path.stat().st_size <= 8 * 1024 ** 2, "Unbounded Segment progress evidence")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--run-directory", required=True, type=Path)
    args = parser.parse_args()
    worker, root = args.worker.resolve(strict=True), args.run_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    try:
        free = shutil.disk_usage(root).free
        recovery.require(free >= 64 * GIB, "G19 requires 64 GiB free before any large fixture/output creation")
        repo = Path(__file__).resolve().parents[2]
        source_seal = {name: hashlib.sha256((repo / name).read_bytes()).hexdigest() for name in SOURCE_FILES}
        provenance = {"arguments": sys.argv, "worker": str(worker), "workerDigests": recovery.file_digests(worker),
                      "sourceSha256": source_seal, "freeBytesBeforeRun": free, "childTimeoutSeconds": 1800,
                      "python": sys.version, "blake3Version": blake3.__version__}
        recovery.write_json(root / "provenance.json", provenance)
        fixture = create_fixture(root)
        source = Path(fixture["sourcePath"])
        print(f"Fixture sealed: {FILE_BYTES} bytes, {SEGMENT_COUNT} Segments, allocated={fixture['actualAllocatedBytes']}; SHA-256={fixture['digests']['sha256']}", flush=True)
        case = root / "transfer"
        case.mkdir()
        before, before_seconds = recovery.run_worker(worker, source, case, "terminated", "large-file-checkpoint",
                                                     "large-file", 218, timeout_seconds=1800)
        journal = recovery.inspect_journal(case / "output")
        recovery.require(journal["completedOrdinals"] == list(range(CHECKPOINT_ORDINAL + 1)), "Unexpected durable checkpoint boundary")
        recovery.require(not (case / "output" / source.name).exists(), "Premature final publication")
        recovery.write_json(case / "before-restart-journal.json", journal)
        frozen = case / "frozen-metadata"
        frozen.mkdir()
        shutil.copyfile(Path(journal["path"]), frozen / "decoder.resume")
        states = list((case / "sender").rglob("runtime.state"))
        recovery.require(len(states) == 1, "Unexpected Encoder runtime state count")
        shutil.copyfile(states[0], frozen / "encoder-runtime.state")
        print(f"Checkpoint terminated: {CHECKPOINT_ORDINAL + 1} durable Segments, {before_seconds:.3f}s; restarting same Session", flush=True)
        after, restart_seconds = recovery.run_worker(worker, source, case, "restarted", mode="large-file", timeout_seconds=1800)
        identity = recovery.verify_ids(before, after, "large-file-checkpoint")
        restore = recovery.read_json(after / "restore.json")
        result = recovery.read_json(after / "result.json")
        recovery.require(restore["completedSegments"] == CHECKPOINT_ORDINAL + 1 and
                         result["restoredVerifiedSegments"] == CHECKPOINT_ORDINAL + 1, "Durable completed state was lost")
        recovery.require(result["schema"] == "PixelBridge.G19LargeFileWorker.1" and result["published"] and
                         result["finalReopenVerified"] and result["verifiedSegments"] == SEGMENT_COUNT and
                         result["verifiedBytes"] == FILE_BYTES, "Authoritative large-file publication failed")
        recovery.require(result["senderResidentSegments"] <= 2 and result["senderResidentEncodedBytes"] <= 2 * SEGMENT_BYTES and
                         result["receiverActiveDecoders"] <= 4 and result["resumeResidentPayloadBytes"] <= 512 * 1024 ** 2 and
                         result["peakWorkingSetBytes"] < 256 * 1024 ** 2, "Large-file bounded working-set contract failed")
        recovery.require(not list((case / "output").glob("*.part")) and not list((case / "output").glob("*.resume")),
                         "Final publication retained active part/resume files")
        progress = read_progress(before / "progress.jsonl") + read_progress(after / "progress.jsonl")
        touched = set()
        peak_journal_bytes = result["peakJournalFileBytes"]
        peak_memory_bytes = max(result["peakWorkingSetBytes"], recovery.read_json(before / "crash.json")["peakWorkingSetBytes"])
        for item in progress:
            ordinal = item["ordinal"]
            recovery.require(0 <= ordinal < SEGMENT_COUNT and item["rawOffset"] == ordinal * SEGMENT_BYTES and
                             item["rawSize"] == min(SEGMENT_BYTES, FILE_BYTES - item["rawOffset"]), "Segment completion range mismatch")
            touched.add(ordinal)
            peak_journal_bytes = max(peak_journal_bytes, item["peakJournalFileBytes"])
            peak_memory_bytes = max(peak_memory_bytes, item["peakWorkingSetBytes"])
        recovery.require(touched == set(range(SEGMENT_COUNT)), "Not every Segment was actually completed through Receiver")
        recovery.require(result["maxOrdinal"] == SEGMENT_COUNT - 1 and result["maxRawOffset"] == 20 * GIB and
                         peak_journal_bytes <= 256 * 1024 ** 2, "Large-file ordinal/offset/journal bound failed")
        external = verify_all_bytes(source, case / "output" / source.name, fixture)
        prescans = []
        for evidence in (before, after):
            initial = recovery.read_json(evidence / "initial.json")
            elapsed = initial["prescanMilliseconds"]
            recovery.require(initial["sourceBytes"] == FILE_BYTES and initial["segmentCount"] == SEGMENT_COUNT and
                             initial["wholeFileBlake3"] == fixture["digests"]["blake3"] and elapsed > 0, "Prescan evidence mismatch")
            prescans.append({"milliseconds": elapsed, "MiBPerSecond": FILE_BYTES / 1024 ** 2 / (elapsed / 1000)})
            recovery.require(recovery.read_json(evidence / "large-output-confirmation.json")["policyUnchanged"], "Large output policy changed")
            boundary = recovery.read_json(evidence / "boundary-500gib.json")
            recovery.require(boundary["metadataOnly"] and boundary["bytes"] == 500 * GIB and boundary["segments"] == 64000 and
                             boundary["sessionSegmentManifestRoundTrip"] and boundary["overLimitAndOverflowRejected"], "500 GiB metadata boundary failed")
        recovery.require(source_seal == {name: hashlib.sha256((repo / name).read_bytes()).hexdigest() for name in SOURCE_FILES} and
                         provenance["workerDigests"] == recovery.file_digests(worker), "Tested source or worker changed during G19")
        report = {"schema": "PixelBridge.G19LargeFileRecovery.1", "allPassed": True, "visualChainCovered": False,
                  "fixture": fixture, "provenance": provenance, "terminatedSeconds": before_seconds,
                  "restartToPublishSeconds": restart_seconds, "identity": identity, "restore": restore,
                  "prescans": prescans, "segmentsActuallyCompleted": len(touched), "maxOrdinal": max(touched),
                  "maxRawOffset": result["maxRawOffset"], "peakJournalFileBytes": peak_journal_bytes,
                  "combinedPeakWorkingSetBytes": peak_memory_bytes, "receiverMemoryEvidence": "combined process is a conservative receiver upper bound",
                  "worker": result, "boundary500GiB": boundary, **external}
        recovery.write_json(root / "report.json", report)
        print(f"G19 PASS: {len(touched)} Segments, max offset={result['maxRawOffset']}, external byte comparison/SHA-256/BLAKE3 equal; {root / 'report.json'}", flush=True)
        return 0
    except Exception as error:
        recovery.write_json(root / "failure.json", {"error": str(error), "traceback": traceback.format_exc()})
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
