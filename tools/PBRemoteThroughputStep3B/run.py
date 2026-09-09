"""Fixed Step3-B experiment. No matrix, screen, product changes or automatic retry."""
from __future__ import annotations
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import time
from fractions import Fraction
from blake3 import blake3
from process_runner import invoke

FRAME_BYTES = 1920 * 1080 * 4
FRAME_COUNT = 30
RAW_BYTES = FRAME_BYTES * FRAME_COUNT
SOURCE_BYTES = 65536
SOURCE_NAME = "step3b-source.bin"
SEED = 0x535445503342
MIB = 1024 * 1024
ENCODER_FLAGS = "threads=1:lookahead-threads=1:sliced-threads=0:asm=0:sync-lookahead=0:rc-lookahead=0:mbtree=0:scenecut=0:open-gop=0:intra-refresh=0:bframes=0:ref=1:keyint=15:min-keyint=1:force-cfr=1:nal-hrd=cbr:vbv-init=0.9:aud=1:repeat-headers=1:deblock=0,0"
COLOR_FILTER = "scale=1920:1080:flags=bilinear+accurate_rnd+bitexact:in_range=full:out_range=limited:out_color_matrix=bt709:out_h_chr_pos=0:out_v_chr_pos=128,format=yuv420p,setsar=1,setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709"
PARAMETERS = {"schema": "PixelBridge.Step3B.Parameters.2", "sourceBytes": SOURCE_BYTES,
    "sourceGenerator": "BLAKE3(UTF8(PixelBridge.Step3B.Source.1) + NUL + LE64(seed) + LE64(blockOrdinal)); 2048 32-byte blocks",
    "sourceSeedHex": f"{SEED:016x}", "frames": 30, "fps": "15/1", "codec": "libx264",
    "profile": "high", "level": "4.0", "pixelFormat": "yuv420p", "color": "Limited-BT709-Left",
    "bitrate": 8000000, "minrate": 8000000, "maxrate": 8000000, "vbvBits": 4000000, "vbvInit": 0.9,
    "gop": 15, "idrOrdinals": [0, 15], "bframes": 0, "refs": 1, "preset": "veryfast", "tune": "zerolatency",
    "x264Params": ENCODER_FLAGS, "filter": COLOR_FILTER, "trials": 2,
    "normalDemodObservations": 93, "processSecondsBudget": 600, "fieldStatus": "NOT_RUN"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def local(value):
    path = Path(value)
    require(path.is_absolute() and len(path.drive) == 2 and ctypes.windll.kernel32.GetDriveTypeW(str(path.anchor)) == 3,
            "Explicit fixed local drive required")
    for component in (*reversed(path.parents), path):
        if component.exists():
            require(not component.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT, "Reparse path rejected")
    return path.resolve()


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, "Duplicate JSON key")
        value[key] = item
    return value


def parse(data):
    def reject(value):
        raise ValueError(f"Non-finite JSON: {value}")
    def finite_float(value):
        parsed = float(value)
        require(math.isfinite(parsed), "Non-finite JSON numeric overflow")
        return parsed
    return json.loads(data, object_pairs_hook=unique_object, parse_constant=reject, parse_float=finite_float)


def read_bytes(path, limit):
    local(path)
    require(path.is_file() and path.stat().st_size <= limit, f"Missing/oversized input: {path}")
    with path.open("rb") as stream:
        value = stream.read(limit + 1)
    require(len(value) <= limit, "Input grew beyond limit")
    return value


def read_json(path):
    return parse(read_bytes(path, MIB).decode("utf-8"))


def tree_identity(root, *, byte_limit=1024*MIB):
    total = entries = 0
    files = []

    def visit(parent, depth):
        nonlocal total, entries
        require(depth <= 16, "Evidence directory depth limit")
        with os.scandir(parent) as iterator:
            for entry in iterator:
                entries += 1
                require(entries <= 2048, "Evidence directory entry limit")
                info = entry.stat(follow_symlinks=False)
                require(not info.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT, "Evidence reparse entry")
                path = Path(entry.path)
                if entry.is_dir(follow_symlinks=False):
                    visit(path, depth+1)
                else:
                    require(entry.is_file(follow_symlinks=False) and len(files) < 1024, "Evidence file count/type limit")
                    total += info.st_size
                    require(total <= byte_limit, "Evidence tree byte limit")
                    item = identity(path, limit=byte_limit)
                    item["path"] = path.relative_to(root).as_posix()
                    files.append(item)
    visit(root, 0)
    return {"bytes": total, "entries": entries, "files": sorted(files, key=lambda item: item["path"])}


def write_json(path, value):
    data = (json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
    require(len(data) <= MIB, "JSON output limit")
    with path.open("xb") as stream:
        stream.write(data)


def identity(path, limit=256*MIB):
    local(path)
    size = path.stat().st_size
    require(path.is_file() and size <= limit, "Identity size limit")
    digest = hashlib.sha256()
    consumed = 0
    with path.open("rb") as stream:
        while block := stream.read(MIB):
            consumed += len(block)
            require(consumed <= size, "Input grew during hashing")
            digest.update(block)
    require(consumed == size == path.stat().st_size, "Input size changed")
    return {"path": str(path), "bytes": size, "sha256": digest.hexdigest()}


def source_bytes():
    domain = b"PixelBridge.Step3B.Source.1\0" + struct.pack("<Q", SEED)
    return b"".join(blake3(domain + struct.pack("<Q", index)).digest() for index in range(2048))


def process(root, name, argv, timeout, **options):
    records = list((root / "logs").glob("*.process.json"))
    require(len(records) < 128, "Process ledger count limit")
    spent = sum(read_json(path)["processingSeconds"] for path in records)
    require(spent < 600, "Trial process budget exhausted; no automatic continuation")
    record = invoke(argv, root / "logs" / f"{name}.log", min(timeout, 600-spent), **options)
    require(not record["failures"] and record.get("returnCode") == 0, f"Child failed: {name}; inspect preserved process/log evidence")
    return record


def copy_codec_runtime(root, ffmpeg, ffprobe, dll_root):
    import pefile
    destination = root / "codec-runtime"
    destination.mkdir()
    pending = [ffmpeg, ffprobe]
    seen = set()
    rows = []
    system_dependencies = set()
    while pending:
        source = pending.pop()
        key = source.name.lower()
        if key in seen:
            continue
        seen.add(key)
        require(len(seen) <= 256, "PE dependency closure limit")
        before = identity(source)
        target = destination / source.name
        with source.open("rb") as src, target.open("xb") as dst:
            shutil.copyfileobj(src, dst, MIB)
        require(identity(source) == before and identity(target)["sha256"] == before["sha256"], "Dependency changed while freezing")
        rows.append({"source": before, "frozen": identity(target)})
        with pefile.PE(str(source), fast_load=True) as image:
            image.parse_data_directories(directories=[1, 13])
            imports = list(getattr(image, "DIRECTORY_ENTRY_IMPORT", [])) + list(getattr(image, "DIRECTORY_ENTRY_DELAY_IMPORT", []))
            for entry in imports:
                name = entry.dll.decode("ascii")
                require(Path(name).name == name, "Non-basename PE dependency")
                found = dll_root / name
                if found.is_file():
                    pending.append(found)
                else:
                    require(name.lower().startswith(("api-ms-", "ext-ms-")) or (Path(os.environ["SystemRoot"])/"System32"/name).is_file(), f"Missing DLL dependency: {name}")
                    system_dependencies.add(name)
    write_json(root / "context/codec-runtime.json", {"files": rows, "systemDependencies": sorted(system_dependencies)})


def rows(path, maximum=30):
    lines = read_bytes(path, 4*MIB).splitlines()
    require(0 < len(lines) <= maximum and all(0 < len(line) <= 65536 for line in lines), "Trace row/count limit")
    return [parse(line.decode("utf-8")) for line in lines]


def fixture(root):
    manifest = read_json(root / "frozen-pixels/fixture.json")
    require(manifest["schema"] == "PixelBridge.Step3B.FrozenPixels.1" and manifest["producer"] == "ProductionEncoderRuntime", "Fixture schema/producer")
    require((manifest["frameCount"], manifest["sourceBytes"], manifest["width"], manifest["height"], manifest["rowPitch"], manifest["pixelFormat"], manifest["file"]) ==
            (30, 65536, 1920, 1080, 7680, "BGRA8", "source.bgra"), "Fixture dimensions/count/format changed")
    require(manifest["sessionIdentity"] == "OS-CSPRNG-frozen-in-pixels" and manifest["colorContract"] == "CanonicalSDR_RGB_full_no_conversion", "Fixture identity/color changed")
    require((manifest["outerFec"], manifest["outerBlockBytes"], manifest["systematicBlockCount"]) == ("WirehairV2", 1314, 50), "Fixture FEC dimensions changed")
    require(manifest["timeBaseNumerator"] == 1 and manifest["timeBaseDenominator"] == 15 and len(manifest["frames"]) == 30, "Fixture timing/count")
    raster = root / "frozen-pixels/source.bgra"
    require(raster.stat().st_size == RAW_BYTES, "Frozen raster byte count")
    with raster.open("rb") as stream:
        for ordinal, frame in enumerate(manifest["frames"]):
            pixels = stream.read(FRAME_BYTES)
            require(frame["ordinal"] == frame["pts"] == ordinal and frame["duration"] == 1, "Frozen ordinal/PTS changed")
            require(len(pixels) == FRAME_BYTES and blake3(pixels).hexdigest() == frame["blake3"], "Frozen pixel digest changed")
        require(not stream.read(1), "Frozen trailing data")
    require(len({frame["blake3"] for frame in manifest["frames"]}) == 30, "Expected 30 distinct production rasters")
    require(read_bytes(root / "source" / SOURCE_NAME, SOURCE_BYTES) == source_bytes(), "Fixed source changed")
    sender = read_json(root / "frozen-pixels/sender-report.json")
    require(sender["sourceWholeFileDigest"] == blake3(source_bytes()).hexdigest() and sender["fileBytes"] == SOURCE_BYTES and
            sender["profile"] == "PB-Unified-SC6-V3" and sender["visualLayoutVersion"] == 10 and sender["configuredLogicalFps"] == 15,
            "Production Sender source/Profile identity mismatch")
    return manifest


def validate_ledger(ledger):
    # Schema and exact field names are frozen against the existing production audit.
    require(ledger["schema"] == "PixelBridge.Step1.SourceLedger.1" and ledger["complete"] is True, "Incomplete production ledger")
    require(ledger["rawBytes"] == 65536 and ledger["encodedBytes"] == 65536 and len(ledger["segments"]) == 1, "64 KiB production ledger mismatch")
    segment = ledger["segments"][0]
    digest = blake3(source_bytes()).hexdigest()
    require(segment == {"ordinal": 0, "rawOffset": 0, "rawBytes": 65536, "encodedBytes": 65536,
            "codec": "Raw", "rawBlake3": digest, "encodedBlake3": digest} and ledger["blake3"] == digest, "Production RAW source or digest differs")


def prepare(root, exe):
    require(not (root / "source").exists() and not (root / "frozen-pixels").exists(), "Preparation is create-only")
    (root / "source").mkdir()
    write_json(root / "context/parameters-v2.json", PARAMETERS)
    source = root / "source" / SOURCE_NAME
    data = source_bytes()
    with source.open("xb") as stream:
        stream.write(data)
    write_json(root / "source/identity.json", {**identity(source), "blake3": blake3(data).hexdigest()})
    write_json(root / "context/tool-exe.json", identity(exe))
    process(root, "source-audit-01", [exe, "--audit", source, root / "source/ledger.json"], 60)
    validate_ledger(read_json(root / "source/ledger.json"))
    process(root, "fixture-01", [exe, "--make-fixture", source, root / "frozen-pixels"], 60,
            output_limits=[(root / "frozen-pixels/source.bgra", 256*MIB)])
    fixture(root)
    write_json(root / "context/frozen-input.json", identity(root / "frozen-pixels/source.bgra"))
    process(root, "raw-prefix-01", [exe, "--raw-prefix", root / "frozen-pixels/source.bgra", root / "raw-prefix"], 30)
    prefix = read_json(root / "raw-prefix/summary.json")
    require(prefix["frames"] == 3 and prefix["prefixLimitReached"] and not prefix["publishedAndReopened"] and not prefix["error"], "Three-frame prefix must not recover")
    process(root, "raw-full-01", [exe, "--raw", root / "frozen-pixels/source.bgra", root / "raw-full"], 90)
    result = analyze_replay(root, root / "raw-full", raw=True)
    require(result["recovered"], "Raw baseline failed; stop before codec")
    write_json(root / "RAW_PROOF.json", {"schema": "PixelBridge.Step3B.RawProof.1", "prefixFrames": 3, "prefixRecovered": False,
        "maximumThreeFrameTransportBytes": 59130, "encodedBytes": 65536, "requiresMultipleVisualFrames": True, "full": result})


def analyze_replay(root, directory, *, raw=False, truth=None):
    summary = read_json(directory / "summary.json")
    require(summary["schema"] == "PixelBridge.Step3B.Run.1" and summary["classification"] == "SyntheticOfflineDiagnostic" and summary["fieldStatus"] == "NOT_RUN", "Replay authority changed")
    require(summary["inputContract"] == "OfflinePixels" and summary["processingDevice"] == "D3D11_WARP" and not summary["mediaOnly"], "Replay input/device changed")
    require(summary["raw"] == raw and not summary["intentionalPrefix"] and summary["reachedEof"] and not summary["prefixLimitReached"] and not summary["error"], "Incomplete/error replay")
    require(all(summary[key] is None for key in ("liveChannelGoodput", "simulatedVerifiedGoodput", "originalCaptureClock")), "Unsupported performance authority")
    trace, pixels = rows(directory / "frames.jsonl"), rows(directory / "pixels.jsonl")
    require(len(trace) == len(pixels) == summary["frames"] == 30, "Replay frame count")
    expected = fixture(root)["frames"]
    for index, (frame, pixel) in enumerate(zip(trace, pixels, strict=True)):
        pts = index if raw else (index * 1000 + 7) // 15
        require(frame["observation"] == index + 1 and pixel["ordinal"] == index and frame["pts"] == pixel["pts"] == pts, "Replay PTS/order changed")
        require(frame["timeBaseNumerator"] == pixel["timeBaseNumerator"] == 1 and frame["timeBaseDenominator"] == pixel["timeBaseDenominator"] == (15 if raw else 1000), "Replay timebase changed")
        require(frame["duration"] == pixel["duration"] and (pixel["duration"] == 1 if raw else pixel["duration"] in (66, 67)), "Replay duration changed")
        require(len(frame["slots"]) == 15 and len(frame["freshnessCurrent"]) == 9, "Diagnostic shape")
        require(len(frame["acceptedPayloadDigests"]) == frame["acceptedBlocks"] == sum(slot[5] for slot in frame["slots"]), "Accepted payload accounting")
        require(len({item[0] for item in frame["acceptedPayloadDigests"]}) == frame["acceptedBlocks"], "Duplicate accepted slot")
        if raw:
            require(pixel["pixelBlake3"] == expected[index]["blake3"], "Raw replay pixel changed")
        if truth is not None:
            reference = truth[index]
            if frame["acceptedBlocks"]:
                require((frame["sessionTag"], frame["frameSequence"]) == (reference["sessionTag"], reference["frameSequence"]), "Wrong accepted frame identity")
            payloads = {item[0]: item for item in reference["acceptedPayloadDigests"]}
            require(all(item == payloads.get(item[0]) for item in frame["acceptedPayloadDigests"]), "False accepted payload relative to same-ordinal raw truth")
    report = summary["receiverReport"]
    publish = report["publish"]
    recovered = summary["publishedAndReopened"]
    require(recovered == publish["published"], "Publication surface disagreement")
    final = directory / "output" / SOURCE_NAME
    independent = None
    if recovered:
        require(all(publish[key] for key in ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published")), "Publication gate incomplete")
        require(local(publish["finalPath"]) == final.resolve(), "Final file path disagreement")
        data = read_bytes(final, SOURCE_BYTES)
        require(data == source_bytes() and blake3(data).hexdigest() == publish["wholeFileDigest"], "Independent final bytes/digest mismatch")
        require(report["recovery"]["verifiedRawBytes"] == SOURCE_BYTES, "Verified byte mismatch")
        independent = {**identity(final), "blake3": blake3(data).hexdigest(), "byteEqual": True}
    else:
        require(not final.exists() and not any(publish[key] for key in ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published")), "Unexpected final file or partial publication claim")
    require(report["unifiedTelemetry"]["uniqueVisualFps"] is None and report["recovery"]["currentVerifiedRawGoodputBytesPerSecond"] is None, "Offline report claims live rate")
    require(report["recovery"]["resumeLoaded"] is False, "Fresh Receiver baseline required")
    return {"recovered": recovered, "independent": independent, "frames": len(trace),
            "acceptedBlocks": sum(frame["acceptedBlocks"] for frame in trace),
            "acceptedFrameIdentities": len({(frame["sessionTag"], frame["frameSequence"]) for frame in trace if frame["receiverUniqueAdmission"]})}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["init", "prepare", "freeze-codec", "codec"])
    parser.add_argument("--root", required=True, type=local)
    parser.add_argument("--exe", type=local)
    parser.add_argument("--ffmpeg", type=local)
    parser.add_argument("--ffprobe", type=local)
    parser.add_argument("--dll-root", type=local)
    args = parser.parse_args()
    if args.command == "init":
        require(not args.root.exists(), "Run root must be new")
        args.root.mkdir()
        (args.root / "context").mkdir()
        (args.root / "logs").mkdir()
        write_json(args.root / "context/preflight.json", {"schema": "PixelBridge.Step3B.ReplayPreflight.1", "scope": PARAMETERS,
            "note": "New local synthetic run; does not authorize a screen, matrix, commit or dependency install"})
        return
    require(args.root.is_dir() and (args.root / "context/preflight.json").is_file(), "Explicit preflight evidence root required")
    if args.command == "prepare":
        require(args.exe is not None, "Explicit tool EXE required")
        prepare(args.root, args.exe)
    elif args.command == "freeze-codec":
        require(all((args.ffmpeg, args.ffprobe, args.dll_root)), "Explicit codec paths required")
        copy_codec_runtime(args.root, args.ffmpeg, args.ffprobe, args.dll_root)
    else:
        from codec import run_codec
        require(args.exe is not None, "Explicit tool EXE required")
        run_codec(args.root, args.exe)


if __name__ == "__main__":
    main()
