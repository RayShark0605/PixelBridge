#!/usr/bin/env python3
"""Build a bounded, deterministic LF4 H.264/HEVC evidence corpus.

The script accepts explicit, existing PBRemoteVisualCodecProbe, ffmpeg and
ffprobe executables. It never discovers a provider, changes codec thresholds or
launches a window. Every subprocess has a timeout and bounded diagnostic
capture. The output directory is create-only and contains source/bitstream/
decoded/evaluation/probe artifacts plus a canonical sealed manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat as stat_module
import subprocess
import sys
import threading
import time
from typing import Any, Iterable

import blake3


SCHEMA = "PixelBridge.RemoteVisualCodecCorpus.2"
SOURCE_REPORT_SCHEMA = "PixelBridge.RemoteVisualCodecSource.1"
EVALUATION_REPORT_SCHEMA = "PixelBridge.RemoteVisualCodecFrameEvaluation.2"
MAX_DIAGNOSTIC_BYTES = 8 * 1024 * 1024
MAX_JSON_BYTES = 8 * 1024 * 1024
MAX_BITSTREAM_BYTES = 256 * 1024 * 1024
MAX_RAW_BYTES = 1920 * 1080 * 4 * 16
COMMAND_TIMEOUT_SECONDS = 300
SOURCE_WIDTH = 1920
SOURCE_HEIGHT = 1080
SOURCE_FPS = 2
SOURCE_FRAMES = 3
SOURCE_FIRST_FRAME_SEQUENCE = 0
SOURCE_PROFILE = "PB-RemoteVisual-LF4-X1"
SOURCE_PROFILE_ID = "504252564c463431"
SOURCE_LAYOUT_VERSION = 7
SOURCE_SESSION_TAG = "5751a0fe3cc27908"
MAX_TRANSPORT_BLOCK_BYTES = 65571
RECEIVER_SEGMENT_BYTES = 4 * 1314


CASES: tuple[dict[str, Any], ...] = (
    {
        "name": "h264-420-crf18-intra",
        "codec": "h264",
        "encoder": "libx264",
        "pixelFormat": "yuv420p",
        "crf": 18,
        "intraOnly": True,
        "colorRange": "tv",
    },
    {
        "name": "h264-420-crf35-inter",
        "codec": "h264",
        "encoder": "libx264",
        "pixelFormat": "yuv420p",
        "crf": 35,
        "intraOnly": False,
        "colorRange": "tv",
    },
    {
        "name": "h264-444-crf28-inter",
        "codec": "h264",
        "encoder": "libx264",
        "pixelFormat": "yuv444p",
        "crf": 28,
        "intraOnly": False,
        "colorRange": "tv",
    },
    {
        "name": "h264-444-full-crf28-inter",
        "codec": "h264",
        "encoder": "libx264",
        "pixelFormat": "yuv444p",
        "inspectedPixelFormat": "yuvj444p",
        "crf": 28,
        "intraOnly": False,
        "colorRange": "pc",
    },
    {
        "name": "hevc-420-crf28-inter",
        "codec": "hevc",
        "encoder": "libx265",
        "pixelFormat": "yuv420p",
        "crf": 28,
        "intraOnly": False,
        "colorRange": "tv",
    },
    {
        "name": "hevc-420-crf40-inter",
        "codec": "hevc",
        "encoder": "libx265",
        "pixelFormat": "yuv420p",
        "crf": 40,
        "intraOnly": False,
        "colorRange": "tv",
    },
)


class CorpusError(RuntimeError):
    pass


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def reject_duplicate_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CorpusError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def reject_non_finite(value: str) -> None:
    raise CorpusError(f"non-finite JSON number: {value}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def blake3_bytes(contents: bytes) -> str:
    return blake3.blake3(contents).hexdigest()


def require_lowercase_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise CorpusError(f"{label} must be a lowercase BLAKE3-256 digest")
    return value


def read_exact_stable_file(path: Path, expected_bytes: int, label: str) -> bytes:
    if expected_bytes <= 0 or path.is_symlink():
        raise CorpusError(f"{label} is not an exact regular file")
    try:
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat_module.S_ISREG(before.st_mode) or before.st_size != expected_bytes:
                raise CorpusError(f"{label} is not an exact regular file")
            contents = stream.read(expected_bytes + 1)
            after = os.fstat(stream.fileno())
        path_after = path.stat()
    except OSError as exception:
        raise CorpusError(f"cannot read {label}: {path}") from exception
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    if len(contents) != expected_bytes or \
            any(getattr(before, field) != getattr(after, field) for field in stable_fields) or \
            any(getattr(after, field) != getattr(path_after, field) for field in stable_fields):
        raise CorpusError(f"{label} changed while being read")
    return contents


def artifact(path: Path, root: Path) -> dict[str, Any]:
    stat = path.stat()
    if not path.is_file():
        raise CorpusError(f"artifact is not a regular file: {path}")
    return {
        "path": path.relative_to(root).as_posix(),
        "bytes": stat.st_size,
        "sha256": sha256_file(path),
    }


def write_new(path: Path, contents: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(contents)
        stream.flush()
        os.fsync(stream.fileno())


def check_executable(path: Path, label: str) -> Path:
    resolved = path.resolve(strict=True)
    if not resolved.is_file():
        raise CorpusError(f"{label} is not a regular file: {resolved}")
    return resolved


def run_bounded(arguments: list[str], *, stdout_path: Path | None = None,
                maximum_stdout_bytes: int = MAX_DIAGNOSTIC_BYTES,
                maximum_stderr_bytes: int = MAX_DIAGNOSTIC_BYTES,
                timeout_seconds: float = COMMAND_TIMEOUT_SECONDS) -> bytes:
    if not arguments or maximum_stdout_bytes < 0 or maximum_stderr_bytes < 0 or timeout_seconds <= 0:
        raise CorpusError("subprocess resource policy is invalid")

    stdout_memory = bytearray()
    stderr_memory = bytearray()
    overflow_event = threading.Event()
    overflow_labels: list[str] = []
    reader_errors: list[BaseException] = []
    output_stream = None
    process: subprocess.Popen[bytes] | None = None

    def drain(stream: Any, maximum_bytes: int, destination: Any, label: str) -> None:
        total_bytes = 0
        try:
            while True:
                chunk = stream.read(64 * 1024)
                if not chunk:
                    break
                remaining = max(0, maximum_bytes - total_bytes)
                if remaining:
                    destination.write(chunk[:remaining]) if hasattr(destination, "write") else destination.extend(chunk[:remaining])
                total_bytes += len(chunk)
                if total_bytes > maximum_bytes and label not in overflow_labels:
                    overflow_labels.append(label)
                    overflow_event.set()
        except BaseException as exception:
            reader_errors.append(exception)
            overflow_event.set()
        finally:
            stream.close()

    succeeded = False
    try:
        if stdout_path is not None:
            output_stream = stdout_path.open("xb")
            stdout_destination: Any = output_stream
        else:
            stdout_destination = stdout_memory
        process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        if process.stdout is None or process.stderr is None:
            raise CorpusError("subprocess pipes were not created")
        stdout_thread = threading.Thread(target=drain,
                                         args=(process.stdout, maximum_stdout_bytes, stdout_destination, "stdout"),
                                         daemon=True)
        stderr_thread = threading.Thread(target=drain,
                                         args=(process.stderr, maximum_stderr_bytes, stderr_memory, "stderr"),
                                         daemon=True)
        stdout_thread.start()
        stderr_thread.start()
        deadline = time.monotonic() + timeout_seconds
        timed_out = False
        while process.poll() is None:
            remaining_seconds = deadline - time.monotonic()
            if remaining_seconds <= 0:
                timed_out = True
                process.kill()
                break
            if overflow_event.wait(min(0.02, remaining_seconds)):
                process.kill()
                break
        process.wait()
        stdout_thread.join()
        stderr_thread.join()
        if output_stream is not None:
            output_stream.flush()
            os.fsync(output_stream.fileno())
        if timed_out:
            raise subprocess.TimeoutExpired(arguments, timeout_seconds, bytes(stdout_memory), bytes(stderr_memory))
        if reader_errors:
            raise CorpusError(f"subprocess diagnostic capture failed: {reader_errors[0]}")
        if overflow_labels:
            label = overflow_labels[0]
            limit = maximum_stdout_bytes if label == "stdout" else maximum_stderr_bytes
            raise CorpusError(f"subprocess {label} exceeds {limit} bytes: {arguments[0]}")
        if process.returncode != 0:
            diagnostic = bytes(stderr_memory).decode("utf-8", errors="replace")[-4096:]
            raise CorpusError(f"subprocess failed with exit {process.returncode}: {arguments}\n{diagnostic}")
        succeeded = True
        return bytes(stdout_memory)
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        if output_stream is not None:
            output_stream.close()
        if stdout_path is not None and not succeeded:
            stdout_path.unlink(missing_ok=True)


def normalize_cli_json(contents: bytes, label: str) -> bytes:
    if b"\r" not in contents:
        return contents
    if contents.endswith(b"\r\n") and b"\r" not in contents[:-2]:
        return contents[:-2] + b"\n"
    raise CorpusError(f"{label} contains a noncanonical carriage return")


def normalize_text_output(contents: bytes, label: str) -> bytes:
    normalized = contents.replace(b"\r\n", b"\n")
    if b"\r" in normalized or b"\x00" in normalized:
        raise CorpusError(f"{label} contains a noncanonical control byte")
    try:
        normalized.decode("utf-8")
    except UnicodeDecodeError as exception:
        raise CorpusError(f"{label} is not UTF-8") from exception
    return normalized


def codec_arguments(ffmpeg: Path, source: Path, output: Path, case: dict[str, Any]) -> list[str]:
    color_range = case["colorRange"]
    encoder_range = "full" if color_range == "pc" else "limited"
    common = [
        str(ffmpeg), "-hide_banner", "-loglevel", "warning", "-nostdin", "-n",
        "-f", "rawvideo", "-pixel_format", "bgra", "-video_size", f"{SOURCE_WIDTH}x{SOURCE_HEIGHT}",
        "-framerate", str(SOURCE_FPS), "-i", str(source), "-frames:v", str(SOURCE_FRAMES),
        "-an", "-sn", "-dn", "-map_metadata", "-1", "-threads", "1",
        "-vf", f"scale=in_range=pc:out_range={color_range}:in_color_matrix=bt709:out_color_matrix=bt709,format={case['pixelFormat']}",
        "-c:v", case["encoder"], "-pix_fmt", case["pixelFormat"], "-crf", str(case["crf"]),
        "-color_range", color_range, "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-fflags", "+bitexact", "-flags:v", "+bitexact", "-fs", str(MAX_BITSTREAM_BYTES),
    ]
    if case["encoder"] == "libx264":
        keyint = 1 if case["intraOnly"] else 30
        x264 = (
            f"keyint={keyint}:min-keyint={keyint}:scenecut=0:bframes=0:threads=1:sync-lookahead=0:"
            "rc-lookahead=0:aq-mode=0:psy=0:mbtree=0:colorprim=bt709:transfer=bt709:"
            f"colormatrix=bt709:range={encoder_range}"
        )
        common.extend(["-x264-params", x264])
        if case["pixelFormat"] == "yuv444p":
            common.extend(["-profile:v", "high444"])
    else:
        keyint = 1 if case["intraOnly"] else 30
        x265 = (
            f"keyint={keyint}:min-keyint={keyint}:scenecut=0:bframes=0:pools=none:frame-threads=1:"
            "lookahead-threads=1:wpp=0:pmode=0:pme=0:aq-mode=0:psy-rd=0:psy-rdoq=0:"
            f"rc-lookahead=0:colorprim=1:transfer=1:colormatrix=1:range={encoder_range}:log-level=error"
        )
        common.extend(["-x265-params", x265])
    common.append(str(output))
    return common


def decode_arguments(ffmpeg: Path, bitstream: Path, output: Path) -> list[str]:
    return [
        str(ffmpeg), "-hide_banner", "-loglevel", "warning", "-nostdin", "-n", "-threads", "1",
        "-i", str(bitstream), "-map", "0:v:0", "-frames:v", str(SOURCE_FRAMES),
        "-an", "-sn", "-dn", "-pix_fmt", "gray", "-f", "rawvideo", "-fs",
        str(SOURCE_WIDTH * SOURCE_HEIGHT * SOURCE_FRAMES), str(output),
    ]


def probe_arguments(ffprobe: Path, bitstream: Path) -> list[str]:
    return [
        str(ffprobe), "-v", "error", "-show_streams", "-show_frames", "-show_packets", "-show_format",
        "-of", "json", str(bitstream),
    ]


def require_object_list(value: Any, label: str) -> list[dict[str, Any]]:
    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        raise CorpusError(f"ffprobe {label} must be an array of objects")
    return value


def extract_probe_frames_and_packets(probe: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    combined_value = probe.get("packets_and_frames")
    if combined_value is not None:
        if "frames" in probe or "packets" in probe:
            raise CorpusError("ffprobe output mixes combined and separate frame/packet arrays")
        combined = require_object_list(combined_value, "packets_and_frames")
        unknown_types = [item.get("type") for item in combined if item.get("type") not in ("frame", "packet")]
        if unknown_types:
            raise CorpusError(f"ffprobe combined output contains unknown record types: {unknown_types}")
        return (
            [item for item in combined if item.get("type") == "frame"],
            [item for item in combined if item.get("type") == "packet"],
        )
    return (
        require_object_list(probe.get("frames", []), "frames"),
        require_object_list(probe.get("packets", []), "packets"),
    )


def normalize_probe_document(probe: dict[str, Any], bitstream: Path, root: Path) -> None:
    format_value = probe.get("format")
    if not isinstance(format_value, dict):
        raise CorpusError("ffprobe format must be an object")
    filename = format_value.get("filename")
    if not isinstance(filename, str):
        raise CorpusError("ffprobe format filename must be a string")
    try:
        observed_path = Path(filename).resolve(strict=True)
    except OSError as exception:
        raise CorpusError(f"ffprobe format filename is not an existing file: {filename}") from exception
    if observed_path != bitstream.resolve(strict=True):
        raise CorpusError(f"ffprobe inspected an unexpected file: {filename}")
    format_value["filename"] = bitstream.relative_to(root).as_posix()


def validate_probe(probe: dict[str, Any], case: dict[str, Any]) -> dict[str, Any]:
    streams = require_object_list(probe.get("streams", []), "streams")
    if len(streams) != 1 or streams[0].get("codec_type") != "video":
        raise CorpusError(f"{case['name']} must contain exactly one stream and it must be video")
    stream = streams[0]
    if stream.get("codec_name") != case["codec"]:
        raise CorpusError(f"{case['name']} codec mismatch: {stream.get('codec_name')}")
    expected_pixel_format = case.get("inspectedPixelFormat", case["pixelFormat"])
    if stream.get("pix_fmt") != expected_pixel_format:
        raise CorpusError(f"{case['name']} pixel format mismatch: {stream.get('pix_fmt')}")
    if require_nonnegative_integer(stream.get("width"), f"{case['name']} stream width") != SOURCE_WIDTH or \
            require_nonnegative_integer(stream.get("height"), f"{case['name']} stream height") != SOURCE_HEIGHT:
        raise CorpusError(f"{case['name']} geometry mismatch")
    expected_ranges = ("pc", "jpeg") if case["colorRange"] == "pc" else ("tv", "mpeg")
    if stream.get("color_space") != "bt709" or stream.get("color_range") not in expected_ranges:
        raise CorpusError(f"{case['name']} color metadata mismatch")
    all_frames, all_packets = extract_probe_frames_and_packets(probe)
    if any(frame.get("media_type") != "video" for frame in all_frames) or \
            any(packet.get("codec_type") != "video" for packet in all_packets):
        raise CorpusError(f"{case['name']} contains a non-video frame or packet")
    frames = [frame for frame in all_frames if frame.get("media_type") == "video"]
    packets = [packet for packet in all_packets if packet.get("codec_type") == "video"]
    if len(frames) != SOURCE_FRAMES or len(packets) != SOURCE_FRAMES:
        raise CorpusError(f"{case['name']} frame/packet count mismatch: {len(frames)}/{len(packets)}")
    if any(frame.get("color_range") not in expected_ranges or frame.get("color_space") != "bt709" or
           frame.get("color_transfer") != "bt709" or frame.get("color_primaries") != "bt709"
           for frame in frames):
        raise CorpusError(f"{case['name']} per-frame color metadata mismatch")
    key_frame_values = [require_nonnegative_integer(frame.get("key_frame"),
                                                    f"{case['name']} frame key_frame") for frame in frames]
    if any(value not in (0, 1) for value in key_frame_values):
        raise CorpusError(f"{case['name']} key_frame must be zero or one")
    key_frames = sum(key_frame_values)
    if case["intraOnly"] and key_frames != SOURCE_FRAMES:
        raise CorpusError(f"{case['name']} intra-only stream has {key_frames} key frames")
    if not case["intraOnly"] and key_frames != 1:
        raise CorpusError(f"{case['name']} inter stream must have exactly one key frame, observed {key_frames}")
    return {
        "codecName": stream.get("codec_name"),
        "profile": stream.get("profile"),
        "level": stream.get("level"),
        "pixelFormat": stream.get("pix_fmt"),
        "colorRange": stream.get("color_range"),
        "colorSpace": stream.get("color_space"),
        "colorTransfer": frames[0].get("color_transfer"),
        "colorPrimaries": frames[0].get("color_primaries"),
        "frameCount": len(frames),
        "packetCount": len(packets),
        "keyFrameCount": key_frames,
        "pictureTypes": [frame.get("pict_type") for frame in frames],
    }


def parse_json_bytes(contents: bytes, label: str) -> dict[str, Any]:
    if len(contents) > MAX_JSON_BYTES:
        raise CorpusError(f"{label} exceeds {MAX_JSON_BYTES} bytes")
    try:
        value = json.loads(contents, object_pairs_hook=reject_duplicate_members,
                           parse_constant=reject_non_finite)
    except (UnicodeDecodeError, json.JSONDecodeError) as exception:
        raise CorpusError(f"{label} is not valid UTF-8 JSON: {exception}") from exception
    if not isinstance(value, dict):
        raise CorpusError(f"{label} must be a JSON object")
    return value


def validate_canonical_report(contents: bytes, schema: str, label: str, expected_version: int = 1) -> dict[str, Any]:
    report = parse_json_bytes(contents, label)
    if list(report) != ["schema", "version", "payloadBlake3", "payload"]:
        raise CorpusError(f"{label} has a noncanonical top-level member order")
    version = report.get("version")
    if report.get("schema") != schema or not isinstance(version, int) or isinstance(version, bool) or \
            version != expected_version or \
            not isinstance(report.get("payload"), dict):
        raise CorpusError(f"{label} schema/version/payload mismatch")
    canonical_report = (json.dumps(report, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    if canonical_report != contents:
        raise CorpusError(f"{label} is not canonical JSON")
    payload_blake3 = report.get("payloadBlake3")
    require_lowercase_digest(payload_blake3, f"{label} payloadBlake3")
    payload_bytes = json.dumps(report["payload"], ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if blake3.blake3(payload_bytes).hexdigest() != payload_blake3:
        raise CorpusError(f"{label} payloadBlake3 mismatch")
    return report


def require_nonnegative_integer(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise CorpusError(f"{label} must be a nonnegative integer")
    return value


def validate_source_report(report: dict[str, Any], source_path: Path) -> None:
    payload = report["payload"]
    expected_keys = ["format", "width", "height", "frameBytes", "frameCount", "profile", "profileId",
                     "layoutVersion", "sessionTag", "sequenceBlake3", "frames"]
    if list(payload) != expected_keys:
        raise CorpusError("source manifest has an unexpected payload shape or member order")
    expected_frame_bytes = SOURCE_WIDTH * SOURCE_HEIGHT * 4
    if payload.get("format") != "bgra8" or \
            require_nonnegative_integer(payload.get("width"), "source width") != SOURCE_WIDTH or \
            require_nonnegative_integer(payload.get("height"), "source height") != SOURCE_HEIGHT or \
            require_nonnegative_integer(payload.get("frameBytes"), "source frameBytes") != expected_frame_bytes or \
            require_nonnegative_integer(payload.get("frameCount"), "source frameCount") != SOURCE_FRAMES or \
            payload.get("profile") != SOURCE_PROFILE or payload.get("profileId") != SOURCE_PROFILE_ID or \
            require_nonnegative_integer(payload.get("layoutVersion"),
                                        "source layoutVersion") != SOURCE_LAYOUT_VERSION or \
            payload.get("sessionTag") != SOURCE_SESSION_TAG:
        raise CorpusError("source manifest geometry, profile or identity mismatch")

    expected_bytes = expected_frame_bytes * SOURCE_FRAMES
    source_bytes = read_exact_stable_file(source_path, expected_bytes, "source BGRA sequence")
    if require_lowercase_digest(payload.get("sequenceBlake3"), "source sequenceBlake3") != \
            blake3_bytes(source_bytes):
        raise CorpusError("source manifest sequenceBlake3 does not bind the exported BGRA bytes")
    frames = payload.get("frames")
    if not isinstance(frames, list) or len(frames) != SOURCE_FRAMES:
        raise CorpusError("source manifest frame list is malformed")
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict) or list(frame) != ["index", "frameSequence", "blake3"] or \
                require_nonnegative_integer(frame.get("index"), f"source frame {index} index") != index or \
                require_nonnegative_integer(frame.get("frameSequence"),
                                            f"source frame {index} frameSequence") != SOURCE_FIRST_FRAME_SEQUENCE + index:
            raise CorpusError(f"source manifest frame {index} identity is malformed")
        start = index * expected_frame_bytes
        expected_digest = blake3_bytes(source_bytes[start:start + expected_frame_bytes])
        if require_lowercase_digest(frame.get("blake3"), f"source frame {index} blake3") != expected_digest:
            raise CorpusError(f"source manifest frame {index} does not bind the exported BGRA bytes")


def validate_evaluation_input(report: dict[str, Any], decoded_path: Path, case_name: str) -> None:
    payload = report["payload"]
    input_value = payload.get("input")
    if not isinstance(input_value, dict) or list(input_value) != \
            ["format", "width", "height", "frameBytes", "frameCount", "byteCount", "blake3"]:
        raise CorpusError(f"{case_name} evaluation input descriptor is malformed")
    expected_frame_bytes = SOURCE_WIDTH * SOURCE_HEIGHT
    expected_bytes = expected_frame_bytes * SOURCE_FRAMES
    decoded_bytes = read_exact_stable_file(decoded_path, expected_bytes, f"{case_name} decoded Gray8 sequence")
    if input_value.get("format") != "gray8" or \
            require_nonnegative_integer(input_value.get("width"), f"{case_name} input width") != SOURCE_WIDTH or \
            require_nonnegative_integer(input_value.get("height"), f"{case_name} input height") != SOURCE_HEIGHT or \
            require_nonnegative_integer(input_value.get("frameBytes"),
                                        f"{case_name} input frameBytes") != expected_frame_bytes or \
            require_nonnegative_integer(input_value.get("frameCount"),
                                        f"{case_name} input frameCount") != SOURCE_FRAMES or \
            require_nonnegative_integer(input_value.get("byteCount"),
                                        f"{case_name} input byteCount") != expected_bytes:
        raise CorpusError(f"{case_name} evaluation input geometry or byte count mismatch")
    if require_lowercase_digest(input_value.get("blake3"), f"{case_name} evaluation input blake3") != \
            blake3_bytes(decoded_bytes):
        raise CorpusError(f"{case_name} evaluation does not bind the decoded Gray8 bytes")
    frames = payload.get("frames")
    if not isinstance(frames, list) or len(frames) != SOURCE_FRAMES:
        raise CorpusError(f"{case_name} evaluation frame list is malformed")
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict):
            raise CorpusError(f"{case_name} evaluation frame {index} is malformed")
        start = index * expected_frame_bytes
        expected_digest = blake3_bytes(decoded_bytes[start:start + expected_frame_bytes])
        if require_lowercase_digest(frame.get("inputBlake3"), f"{case_name} frame {index} inputBlake3") != \
                expected_digest:
            raise CorpusError(f"{case_name} evaluation frame {index} does not bind the decoded Gray8 bytes")


def validate_frame_evaluation(value: Any, label: str, diagnostic: bool,
                              allow_unevaluated: bool = False) -> tuple[int, int]:
    expected_keys = ["evaluated", "paddingValid", "codewords", "fecFailures", "crcFailures",
                     "identityFailures", "falseAcceptedCodewords", "acceptedTransportBlocks",
                     "acceptedRemoteControlBlocks", "iterationsTotal", "iterationsMaximum",
                     "comparedCodedBits", "erroneousCodedBits"]
    if not isinstance(value, dict) or list(value) != expected_keys or not isinstance(value.get("evaluated"), bool) or \
            not isinstance(value.get("paddingValid"), bool):
        raise CorpusError(f"{label} has a malformed evaluation object")
    integers = {key: require_nonnegative_integer(value.get(key), f"{label} {key}")
                for key in expected_keys[2:]}
    if value["evaluated"] is False:
        if not allow_unevaluated or value["paddingValid"] is not False or any(integers.values()):
            raise CorpusError(f"{label} has an invalid unevaluated state")
        return 0, 0
    codewords = integers["codewords"]
    accepted = integers["acceptedTransportBlocks"]
    false_accepted = integers["falseAcceptedCodewords"]
    terminal = accepted + integers["acceptedRemoteControlBlocks"] + integers["fecFailures"] + \
        integers["crcFailures"] + integers["identityFailures"]
    if codewords != 4 or accepted > codewords or terminal != codewords or \
            integers["erroneousCodedBits"] > integers["comparedCodedBits"]:
        raise CorpusError(f"{label} violates the LF4 terminal-accounting contract")
    if diagnostic:
        if integers["comparedCodedBits"] != 64800:
            raise CorpusError(f"{label} lacks the diagnostic coded-bit truth denominator")
    elif false_accepted != 0 or integers["comparedCodedBits"] != 0 or integers["erroneousCodedBits"] != 0:
        raise CorpusError(f"{label} confuses unavailable production truth scoring with diagnostic zero")
    return accepted, false_accepted


def validate_receiver_evidence(value: Any, label: str, expected_segments: int,
                               expected_input_blocks: int) -> dict[str, Any]:
    expected_keys = ["configuredSegments", "inputTransportBlocks", "parsedTransportBlocks",
                     "uniqueOuterSymbols", "identicalDuplicateOuterSymbols", "recoveryReadyOuterSymbols",
                     "alreadyCompletedOuterSymbols", "receiverRejections", "outerConflictRejections",
                     "resourcePolicyRejections", "verifiedSegments", "verifiedRawBytes",
                     "finalizationPrepared", "wholeFileDigestDisposition", "expectedWholeFileBlake3",
                     "observedWholeFileBlake3", "safe"]
    if not isinstance(value, dict) or list(value) != expected_keys:
        raise CorpusError(f"{label} has a malformed Receiver evidence object")
    integers = {key: require_nonnegative_integer(value.get(key), f"{label} {key}")
                for key in expected_keys[:12]}
    configured_segments = integers["configuredSegments"]
    input_blocks = integers["inputTransportBlocks"]
    parsed_blocks = integers["parsedTransportBlocks"]
    unique_outer = integers["uniqueOuterSymbols"]
    duplicate_outer = integers["identicalDuplicateOuterSymbols"]
    recovery_ready = integers["recoveryReadyOuterSymbols"]
    already_completed = integers["alreadyCompletedOuterSymbols"]
    verified_segments = integers["verifiedSegments"]
    verified_raw_bytes = integers["verifiedRawBytes"]
    expected_digest = require_lowercase_digest(value.get("expectedWholeFileBlake3"),
                                               f"{label} expectedWholeFileBlake3")
    observed_digest = value.get("observedWholeFileBlake3")
    if configured_segments != expected_segments or input_blocks != expected_input_blocks or \
            parsed_blocks != input_blocks or unique_outer + duplicate_outer + recovery_ready + already_completed != input_blocks or \
            integers["receiverRejections"] != 0 or integers["outerConflictRejections"] != 0 or \
            integers["resourcePolicyRejections"] != 0 or verified_segments > configured_segments or \
            verified_raw_bytes != verified_segments * RECEIVER_SEGMENT_BYTES or value.get("safe") is not True:
        raise CorpusError(f"{label} violates Receiver/Outer accounting or safety")
    if verified_segments == configured_segments:
        if value.get("finalizationPrepared") is not True or value.get("wholeFileDigestDisposition") != "Pass" or \
                require_lowercase_digest(observed_digest, f"{label} observedWholeFileBlake3") != expected_digest:
            raise CorpusError(f"{label} did not reproduce its completed WholeFileDigest")
    elif value.get("finalizationPrepared") is not False or value.get("wholeFileDigestDisposition") != "NotReady" or \
            observed_digest is not None:
        raise CorpusError(f"{label} claims a digest before all Segments were verified")
    return value


def validate_evaluation_report(report: dict[str, Any], case_name: str) -> dict[str, Any]:
    payload = report["payload"]
    frames = payload.get("frames")
    summary = payload.get("summary")
    if not isinstance(frames, list) or len(frames) != SOURCE_FRAMES or not isinstance(summary, dict):
        raise CorpusError(f"{case_name} evaluation has a malformed frame list or summary")
    classifications = {"Verified": 0, "ErasureNoFalseAccept": 0, "RejectedNoFalseAccept": 0}
    diagnostic_accepted_transport_blocks = 0
    production_accepted_transport_blocks = 0
    false_accepted_codewords = 0
    diagnostic_false_candidates = 0
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict) or require_nonnegative_integer(frame.get("index"),
                f"{case_name} frame index") != index or frame.get("classification") not in classifications:
            raise CorpusError(f"{case_name} evaluation frame {index} is malformed")
        classification = frame["classification"]
        classifications[classification] += 1
        diagnostic_evaluation = frame.get("diagnosticTruthEvaluation")
        production_evaluation = frame.get("productionTransportEvaluation")
        diagnostic_accepted, false_accepted = validate_frame_evaluation(
            diagnostic_evaluation, f"{case_name} frame {index} diagnostic", True, True)
        production_accepted, production_false_accepted = validate_frame_evaluation(
            production_evaluation, f"{case_name} frame {index} production", False, True)
        if production_false_accepted != 0:
            raise CorpusError(f"{case_name} evaluation frame {index} reports impossible production truth scoring")
        if diagnostic_evaluation["evaluated"] is not production_evaluation["evaluated"] or \
                (diagnostic_evaluation["evaluated"] is False and classification != "ErasureNoFalseAccept") or \
                (classification == "Verified" and (diagnostic_accepted != 4 or production_accepted != 4)):
            raise CorpusError(f"{case_name} frame {index} evaluation state contradicts its classification")
        accepted_blocks = frame.get("productionAcceptedBlocks")
        frame_diagnostic_candidates = require_nonnegative_integer(frame.get("diagnosticFalseCandidates"),
                                                                  f"{case_name} diagnosticFalseCandidates")
        frame_false_accepted = require_nonnegative_integer(frame.get("falseAcceptedCodewords"),
                                                           f"{case_name} falseAcceptedCodewords")
        if frame_diagnostic_candidates != false_accepted or frame_false_accepted != 0:
            raise CorpusError(f"{case_name} frame {index} confuses diagnostic candidates with production acceptance")
        if not isinstance(accepted_blocks, list) or len(accepted_blocks) != production_accepted:
            raise CorpusError(f"{case_name} evaluation frame {index} accepted-block evidence is inconsistent")
        observed_slots: set[int] = set()
        for block_index, block in enumerate(accepted_blocks):
            if not isinstance(block, dict) or list(block) != ["slot", "byteCount", "blake3"]:
                raise CorpusError(f"{case_name} frame {index} accepted block {block_index} is malformed")
            slot = require_nonnegative_integer(block.get("slot"), f"{case_name} frame {index} block slot")
            byte_count = require_nonnegative_integer(block.get("byteCount"),
                                                     f"{case_name} frame {index} block byteCount")
            require_lowercase_digest(block.get("blake3"), f"{case_name} frame {index} block blake3")
            if slot >= 4 or slot in observed_slots or not 1 <= byte_count <= MAX_TRANSPORT_BLOCK_BYTES:
                raise CorpusError(f"{case_name} frame {index} accepted block {block_index} violates bounds or identity")
            observed_slots.add(slot)
        diagnostic_accepted_transport_blocks += diagnostic_accepted
        production_accepted_transport_blocks += production_accepted
        diagnostic_false_candidates += frame_diagnostic_candidates
        false_accepted_codewords += frame_false_accepted

    validate_receiver_evidence(payload.get("receiverEvidence"), f"{case_name} receiverEvidence",
                               SOURCE_FRAMES, production_accepted_transport_blocks)

    frame_count = require_nonnegative_integer(summary.get("frameCount"), f"{case_name} frameCount")
    verified_frames = require_nonnegative_integer(summary.get("verifiedFrames"), f"{case_name} verifiedFrames")
    erasure_frames = require_nonnegative_integer(summary.get("erasureFrames"), f"{case_name} erasureFrames")
    rejected_frames = require_nonnegative_integer(summary.get("rejectedFrames"), f"{case_name} rejectedFrames")
    summary_diagnostic_accepted = require_nonnegative_integer(summary.get("diagnosticAcceptedTransportBlocks"),
                                                              f"{case_name} diagnosticAcceptedTransportBlocks")
    summary_production_accepted = require_nonnegative_integer(summary.get("productionAcceptedTransportBlocks"),
                                                              f"{case_name} productionAcceptedTransportBlocks")
    summary_false_accepted = require_nonnegative_integer(summary.get("falseAcceptedCodewords"),
                                                         f"{case_name} falseAcceptedCodewords")
    summary_diagnostic_candidates = require_nonnegative_integer(summary.get("diagnosticFalseCandidates"),
                                                                f"{case_name} diagnosticFalseCandidates")
    if frame_count != SOURCE_FRAMES or verified_frames != classifications["Verified"] or \
            erasure_frames != classifications["ErasureNoFalseAccept"] or \
            rejected_frames != classifications["RejectedNoFalseAccept"] or \
            verified_frames + erasure_frames + rejected_frames != frame_count or \
            summary_diagnostic_accepted != diagnostic_accepted_transport_blocks or \
            summary_production_accepted != production_accepted_transport_blocks or \
            summary_diagnostic_candidates != diagnostic_false_candidates or \
            summary_false_accepted != false_accepted_codewords or \
            summary_false_accepted != 0 or summary.get("truthBoundaryValid") is not True or \
            summary.get("allFramesVerified") is not (verified_frames == frame_count):
        raise CorpusError(f"{case_name} failed the production truth boundary or summary consistency check")
    return summary


def tool_identity(path: Path) -> dict[str, Any]:
    return {"name": path.name, "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def normalized_command(arguments: Iterable[str], tools: dict[str, Path], root: Path) -> list[str]:
    replacements = {str(value): f"<{name}>" for name, value in tools.items()}
    replacements[str(root)] = "<corpus-root>"
    normalized: list[str] = []
    for argument in arguments:
        value = argument
        for original, replacement in replacements.items():
            value = value.replace(original, replacement)
        normalized.append(value.replace("\\", "/"))
    return normalized


def build_corpus(codec_probe: Path, ffmpeg: Path, ffprobe: Path, output_dir: Path) -> dict[str, Any]:
    codec_probe = check_executable(codec_probe, "codec probe")
    ffmpeg = check_executable(ffmpeg, "ffmpeg")
    ffprobe = check_executable(ffprobe, "ffprobe")
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=False, exist_ok=False)
    tools = {"codec-probe": codec_probe, "ffmpeg": ffmpeg, "ffprobe": ffprobe}
    initial_tools = {name: tool_identity(path) for name, path in tools.items()}

    version_artifacts: dict[str, dict[str, Any]] = {}
    for name in ("ffmpeg", "ffprobe"):
        version_bytes = normalize_text_output(
            run_bounded([str(tools[name]), "-version"], maximum_stdout_bytes=MAX_DIAGNOSTIC_BYTES),
            f"{name} version")
        version_path = output_dir / f"{name}-version.txt"
        write_new(version_path, version_bytes)
        version_artifacts[name] = artifact(version_path, output_dir)

    source_path = output_dir / "source-lf4-3f.bgra"
    expected_source_bytes = SOURCE_WIDTH * SOURCE_HEIGHT * 4 * SOURCE_FRAMES
    run_bounded([str(codec_probe), "export-bgra-sequence"], stdout_path=source_path,
                maximum_stdout_bytes=expected_source_bytes)
    if source_path.stat().st_size != expected_source_bytes:
        raise CorpusError("codec probe emitted an unexpected source byte count")
    source_manifest_bytes = normalize_cli_json(
        run_bounded([str(codec_probe), "describe-source"], maximum_stdout_bytes=MAX_JSON_BYTES),
        "source manifest")
    source_manifest = validate_canonical_report(source_manifest_bytes, SOURCE_REPORT_SCHEMA, "source manifest")
    validate_source_report(source_manifest, source_path)
    source_manifest_path = output_dir / "source-manifest.json"
    write_new(source_manifest_path, source_manifest_bytes)

    case_manifests: list[dict[str, Any]] = []
    for case in CASES:
        case_dir = output_dir / case["name"]
        case_dir.mkdir()
        bitstream_path = case_dir / "encoded.mkv"
        encode = codec_arguments(ffmpeg, source_path, bitstream_path, case)
        run_bounded(encode)
        if not bitstream_path.is_file() or bitstream_path.stat().st_size == 0 or \
                bitstream_path.stat().st_size > MAX_BITSTREAM_BYTES:
            raise CorpusError(f"{case['name']} bitstream size is invalid")

        decoded_path = case_dir / "decoded.gray"
        decode = decode_arguments(ffmpeg, bitstream_path, decoded_path)
        run_bounded(decode)
        expected_decoded_bytes = SOURCE_WIDTH * SOURCE_HEIGHT * SOURCE_FRAMES
        if decoded_path.stat().st_size != expected_decoded_bytes:
            raise CorpusError(f"{case['name']} decoded byte count is invalid")

        probe = probe_arguments(ffprobe, bitstream_path)
        probe_bytes = run_bounded(probe, maximum_stdout_bytes=MAX_JSON_BYTES)
        probe_json = parse_json_bytes(probe_bytes, f"{case['name']} ffprobe")
        inspection = validate_probe(probe_json, case)
        normalize_probe_document(probe_json, bitstream_path, output_dir)
        probe_path = case_dir / "ffprobe.json"
        write_new(probe_path, canonical_json(probe_json))

        evaluation_arguments = [str(codec_probe), "evaluate-gray8-sequence", str(decoded_path)]
        evaluation_bytes = normalize_cli_json(
            run_bounded(evaluation_arguments, maximum_stdout_bytes=MAX_JSON_BYTES),
            f"{case['name']} evaluation")
        evaluation = validate_canonical_report(evaluation_bytes, EVALUATION_REPORT_SCHEMA,
                                               f"{case['name']} evaluation", expected_version=2)
        validate_evaluation_input(evaluation, decoded_path, case["name"])
        summary = validate_evaluation_report(evaluation, case["name"])
        evaluation_path = case_dir / "evaluation.json"
        write_new(evaluation_path, evaluation_bytes)

        case_manifests.append({
            "name": case["name"],
            "codec": case["codec"],
            "encoder": case["encoder"],
            "pixelFormat": case["pixelFormat"],
            "inspectedPixelFormat": case.get("inspectedPixelFormat", case["pixelFormat"]),
            "crf": case["crf"],
            "intraOnly": case["intraOnly"],
            "colorRange": case["colorRange"],
            "encodeCommand": normalized_command(encode, tools, output_dir),
            "decodeCommand": normalized_command(decode, tools, output_dir),
            "probeCommand": normalized_command(probe, tools, output_dir),
            "evaluationCommand": normalized_command(evaluation_arguments, tools, output_dir),
            "inspection": inspection,
            "evaluationSummary": summary,
            "receiverEvidence": evaluation["payload"]["receiverEvidence"],
            "artifacts": {
                "bitstream": artifact(bitstream_path, output_dir),
                "decoded": artifact(decoded_path, output_dir),
                "ffprobe": artifact(probe_path, output_dir),
                "evaluation": artifact(evaluation_path, output_dir),
            },
        })

    final_tools = {name: tool_identity(path) for name, path in tools.items()}
    if final_tools != initial_tools:
        raise CorpusError("one or more tool executables changed during corpus generation")
    manifest = {
        "schema": SCHEMA,
        "version": 2,
        "source": {
            "width": SOURCE_WIDTH,
            "height": SOURCE_HEIGHT,
            "fps": SOURCE_FPS,
            "frameCount": SOURCE_FRAMES,
            "artifacts": {
                "bgra": artifact(source_path, output_dir),
                "manifest": artifact(source_manifest_path, output_dir),
            },
            "sourceManifestPayloadBlake3": source_manifest.get("payloadBlake3"),
        },
        "tools": initial_tools,
        "toolVersionArtifacts": version_artifacts,
        "cases": case_manifests,
        "constraints": {
            "timeoutSecondsPerProcess": COMMAND_TIMEOUT_SECONDS,
            "maximumDiagnosticBytes": MAX_DIAGNOSTIC_BYTES,
            "maximumBitstreamBytes": MAX_BITSTREAM_BYTES,
            "maximumRawBytes": MAX_RAW_BYTES,
            "displayOrCaptureUsed": False,
            "providerSpecificThresholds": False,
        },
    }
    manifest_path = output_dir / "codec-corpus-manifest.json"
    write_new(manifest_path, canonical_json(manifest))
    sums: list[str] = []
    for path in sorted(item for item in output_dir.rglob("*") if item.is_file() and item.name != "SHA256SUMS.txt"):
        sums.append(f"{sha256_file(path)}  {path.relative_to(output_dir).as_posix()}")
    write_new(output_dir / "SHA256SUMS.txt", ("\n".join(sums) + "\n").encode("ascii"))
    return manifest


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codec-probe", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--ffprobe", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args(arguments)


def main(arguments: list[str]) -> int:
    options = parse_arguments(arguments)
    try:
        manifest = build_corpus(options.codec_probe, options.ffmpeg, options.ffprobe, options.output_dir)
    except (CorpusError, OSError, subprocess.TimeoutExpired) as exception:
        print(f"[error] {exception}", file=sys.stderr)
        return 1
    summary = {
        "schema": manifest["schema"],
        "outputDir": str(options.output_dir.resolve()),
        "caseCount": len(manifest["cases"]),
        "cases": [
            {
                "name": case["name"],
                "verifiedFrames": case["evaluationSummary"]["verifiedFrames"],
                "erasureFrames": case["evaluationSummary"]["erasureFrames"],
                "falseAcceptedCodewords": case["evaluationSummary"]["falseAcceptedCodewords"],
            }
            for case in manifest["cases"]
        ],
    }
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
