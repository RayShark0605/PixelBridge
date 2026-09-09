"""The single approved codec condition and structural/byte-budget inspectors."""
from __future__ import annotations
import os
from pathlib import Path
import re
from fractions import Fraction
from run import (PARAMETERS, ENCODER_FLAGS, COLOR_FILTER, MIB, RAW_BYTES, require, read_json,
                 read_bytes, write_json, identity, fixture, process, rows, analyze_replay)


def environment(runtime):
    value = dict(os.environ)
    value["PATH"] = str(runtime) + os.pathsep + str(Path(os.environ["SystemRoot"]) / "System32")
    return value


def encode_arguments(ffmpeg, source, output):
    return [ffmpeg, "-nostdin", "-n", "-hide_banner", "-loglevel", "info", "-cpuflags", "0", "-max_alloc", "33554432",
        "-threads", "1", "-protocol_whitelist", "file", "-f", "rawvideo", "-pixel_format", "bgra", "-video_size", "1920x1080",
        "-framerate", "15", "-i", source, "-map", "0:v:0", "-an", "-sn", "-dn", "-map_metadata", "-1", "-map_chapters", "-1",
        "-filter_threads", "1", "-vf", COLOR_FILTER, "-frames:v", "30", "-fps_mode", "passthrough", "-enc_time_base", "1/15",
        "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-profile:v", "high", "-level:v", "4.0",
        "-pix_fmt", "yuv420p", "-color_range", "tv", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-chroma_sample_location", "left", "-threads:v", "1", "-g", "15", "-keyint_min", "1", "-sc_threshold", "0", "-bf", "0", "-refs", "1",
        "-b:v", "8000000", "-minrate", "8000000", "-maxrate", "8000000", "-bufsize", "4000000", "-x264-params", ENCODER_FLAGS,
        "-fflags", "+bitexact", "-flags:v", "+bitexact", "-f", "matroska", output]


def values(text, field):
    return [int(value) for value in re.findall(r"\b" + re.escape(field) + r"\s+[01]+\s+=\s+(-?\d+)", text)]


def fixed_field(text, field, expected):
    found = values(text, field)
    require(found and set(found) == {expected}, f"Bitstream field {field}: expected {expected}, got {sorted(set(found))}")
    return found


def inspect_data(metadata, pictures, headers, encode_log):
    require(len(metadata["streams"]) == 1, "Exactly one stream required")
    stream = metadata["streams"][0]
    expected = {"codec_name": "h264", "codec_type": "video", "profile": "High", "level": 40,
        "width": 1920, "height": 1080, "pix_fmt": "yuv420p", "sample_aspect_ratio": "1:1",
        "color_range": "tv", "color_space": "bt709", "color_transfer": "bt709", "color_primaries": "bt709",
        "chroma_location": "left", "r_frame_rate": "15/1", "avg_frame_rate": "15/1", "time_base": "1/1000", "has_b_frames": 0}
    require(all(stream.get(key) == value for key, value in expected.items()), "Stream codec/geometry/color/timing contract mismatch")
    require(metadata["format"]["format_name"] == "matroska,webm" and Fraction(metadata["format"]["duration"]) == 2, "Container/duration mismatch")
    packets = metadata["packets"]
    frames = pictures["frames"]
    require(len(packets) == len(frames) == 30, "Exact packet/frame count required")
    for index, (packet, frame) in enumerate(zip(packets, frames, strict=True)):
        expected_pts = (index * 1000 + 7) // 15
        require(packet["pts"] == packet["dts"] == frame["pts"] == expected_pts, "Actual PTS/DTS mapping changed")
        require(int(packet["duration"]) in (66, 67) and int(frame["duration"]) in (66, 67), "Packet/frame duration changed")
        require(("K" in packet["flags"]) == (index in (0, 15)), "Packet keyframe cadence mismatch")
        require(frame["key_frame"] == int(index in (0, 15)) and frame["pict_type"] == ("I" if index in (0, 15) else "P"), "Actual I/P structure changed")
        require(frame["width"] == 1920 and frame["height"] == 1080 and frame["pix_fmt"] == "yuv420p" and frame["interlaced_frame"] == 0, "Decoded geometry/format changed")
        require(all(frame.get(key) == value for key, value in expected.items() if key in ("color_range", "color_space", "color_transfer", "color_primaries", "chroma_location")), "Decoded color metadata mismatch")
        require(re.fullmatch(r"SHA256:[0-9a-f]{64}", packet.get("data_hash", "")) is not None, "Missing packet data hash")
    fields = {"profile_idc": 100, "level_idc": 40, "chroma_format_idc": 1, "bit_depth_luma_minus8": 0,
        "bit_depth_chroma_minus8": 0, "frame_mbs_only_flag": 1, "video_full_range_flag": 0, "colour_primaries": 1,
        "transfer_characteristics": 1, "matrix_coefficients": 1, "nal_hrd_parameters_present_flag": 1, "cpb_cnt_minus1": 0,
        "cbr_flag[0]": 1, "fixed_frame_rate_flag": 1, "max_num_ref_frames": 1}
    for key, value in fields.items():
        fixed_field(headers, key, value)
    # x264 omits chroma location syntax for location 0. H264 infers location 0
    # in that case; both independently decoded frame and stream must say left.
    locations = values(headers, "chroma_loc_info_present_flag")
    require(locations and len(set(locations)) == 1, "Ambiguous chroma location syntax")
    if locations[0] == 1:
        fixed_field(headers, "chroma_sample_loc_type_top_field", 0)
        fixed_field(headers, "chroma_sample_loc_type_bottom_field", 0)
    else:
        require(locations[0] == 0 and not values(headers, "chroma_sample_loc_type_top_field") and not values(headers, "chroma_sample_loc_type_bottom_field"), "Invalid inferred chroma location")
    rate_scales = values(headers, "bit_rate_scale")
    size_scales = values(headers, "cpb_size_scale")
    rate_values = values(headers, "bit_rate_value_minus1[0]")
    size_values = values(headers, "cpb_size_value_minus1[0]")
    require(rate_scales and size_scales and len(rate_scales) == len(rate_values) and len(size_scales) == len(size_values), "Missing/ambiguous HRD dimensions")
    require(len(rate_scales) <= 64 and len(size_scales) <= 64 and all(0 <= scale <= 15 for scale in rate_scales+size_scales) and
            all(0 <= value < 2**32 for value in rate_values+size_values), "HRD arithmetic bounds")
    require({(value + 1) << (6 + scale) for value, scale in zip(rate_values, rate_scales)} == {8000000}, "HRD bit rate mismatch")
    require({(value + 1) << (4 + scale) for value, scale in zip(size_values, size_scales)} == {4000000}, "HRD CPB size mismatch")
    nal_types = values(headers, "nal_unit_type")
    require(nal_types.count(5) == 2 and nal_types.count(1) == 28, "Actual IDR/non-IDR slice count changed")
    require("rc=cbr" in encode_log and "vbv_maxrate=8000" in encode_log and "vbv_bufsize=4000" in encode_log and "nal_hrd=cbr" in encode_log, "Effective x264 rate-control settings missing")
    require(not re.search(r"VBV underflow|Error parsing|Error setting|Conversion failed|Unknown option|not supported", encode_log, re.I), "Encoder reported parameter/VBV failure")
    sizes = [int(packet["size"]) for packet in packets]
    require(all(0 < size <= 16*MIB for size in sizes), "Packet size bound")
    # Necessary engineering envelope, not a full Annex C HRD conformance test.
    for start in range(30):
        bits = 0
        for end in range(start, 30):
            bits += sizes[end] * 8
            duration = Fraction(packets[end]["pts"] - packets[start]["pts"] + int(packets[end]["duration"]), 1000)
            require(bits <= 8000000 * duration + 4000000, "Packet-byte VBV envelope exceeded")
    return {"schema": "PixelBridge.Step3B.CodecInspection.1", "passed": True, "stream": expected,
        "frames": 30, "idrOrdinals": [0, 15], "nonIdrPPictures": 28, "hrdBitrate": 8000000, "hrdCpbBits": 4000000,
        "videoPacketBytes": sum(sizes), "videoPacketAverageBitrate": sum(sizes)*8/2,
        "peakPacketBytes": max(sizes), "containerBytes": int(metadata["format"]["size"]),
        "containerAverageBitrate": int(metadata["format"]["size"])*8/2, "fillerNalCount": nal_types.count(12),
        "packetEnvelopePassed": True, "fullHrdConformanceCertified": False,
        "offlineArtifactExpansionRatio": int(metadata["format"]["size"])/65536,
        "modelDurationSeconds": 2, "actualPtsSpanSeconds": Fraction(packets[-1]["pts"]-packets[0]["pts"],1000).__float__()}


def inspect(root, trial, ffmpeg, ffprobe):
    stream_path = trial / "channel.mkv"
    env = environment(ffmpeg.parent)
    base = [ffprobe, "-v", "error", "-cpuflags", "0", "-max_alloc", "33554432", "-threads", "1", "-protocol_whitelist", "file"]
    process(root, f"{trial.name}-packets", base + ["-show_streams", "-show_format", "-show_packets", "-show_data_hash", "sha256", "-of", "json", stream_path], 15, env=env)
    process(root, f"{trial.name}-pictures", base + ["-show_frames", "-of", "json", stream_path], 15, env=env)
    process(root, f"{trial.name}-headers", [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info", "-cpuflags", "0", "-max_alloc", "33554432",
        "-threads", "1", "-protocol_whitelist", "file", "-i", stream_path, "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "NUL"], 15, env=env, limit=8*MIB)
    metadata = read_json(root / "logs" / f"{trial.name}-packets.log")
    pictures = read_json(root / "logs" / f"{trial.name}-pictures.log")
    headers = read_bytes(root / "logs" / f"{trial.name}-headers.log", 8*MIB).decode("utf-8", errors="replace")
    encoding = read_bytes(root / "logs" / f"{trial.name}-encode.log", MIB).decode("utf-8", errors="replace")
    result = inspect_data(metadata, pictures, headers, encoding)
    write_json(trial / "inspection.json", result)
    return result


def run_codec(root, exe):
    require(read_json(root / "context/parameters-v2.json") == PARAMETERS, "Approved parameters changed")
    require(read_json(root / "RAW_PROOF.json")["full"]["recovered"], "Raw proof required first")
    require(identity(exe) == read_json(root / "context/tool-exe.json"), "Tool EXE changed since raw proof")
    fixture(root)
    require(identity(root / "frozen-pixels/source.bgra") == read_json(root / "context/frozen-input.json"), "Frozen input identity changed")
    runtime = root / "codec-runtime"
    for item in read_json(root / "context/codec-runtime.json")["files"]:
        require(identity(Path(item["frozen"]["path"])) == item["frozen"], "Frozen codec dependency changed")
    ffmpeg, ffprobe = runtime / "ffmpeg.exe", runtime / "ffprobe.exe"
    truth = rows(root / "raw-full/frames.jsonl")
    results = []
    for number in (1, 2):
        trial = root / f"codec-v2-{number:02}"
        trial.mkdir()
        argv = encode_arguments(ffmpeg, root / "frozen-pixels/source.bgra", trial / "channel.mkv")
        process(root, f"{trial.name}-encode", argv, 60, env=environment(runtime), output_limits=[(trial / "channel.mkv", 16*MIB)])
        write_json(trial / "bitstream-identity.json", identity(trial / "channel.mkv", 16*MIB))
        inspected = inspect(root, trial, ffmpeg, ffprobe)
        process(root, f"{trial.name}-replay", [exe, "--codec", trial / "channel.mkv", trial / "replay"], 90)
        analysis = analyze_replay(root, trial / "replay", truth=truth)
        write_json(trial / "recovery-analysis.json", analysis)
        results.append({"inspection": inspected, "recovery": analysis})
    left, right = root / "codec-v2-01", root / "codec-v2-02"
    for name in ("frames.jsonl", "pixels.jsonl"):
        first, second = rows(left / "replay" / name), rows(right / "replay" / name)
        normalized = lambda records: [{key: value for key, value in row.items() if key != "processingQpc100ns"} for row in records]
        require(normalized(first) == normalized(second), f"Non-clock replay mismatch: {name}")
    first_packets = read_json(root / "logs/codec-v2-01-packets.log")["packets"]
    second_packets = read_json(root / "logs/codec-v2-02-packets.log")["packets"]
    require(first_packets == second_packets, "Encoded packet data/order/timing mismatch")
    require(results[0]["inspection"] == results[1]["inspection"], "Codec inspection results differ")
    require(identity(root / "frozen-pixels/source.bgra") == read_json(root / "context/frozen-input.json") and identity(exe) == read_json(root / "context/tool-exe.json"), "Input/tool changed during codec trial")
    write_json(root / "CODEC_RESULT.json", {"schema": "PixelBridge.Step3B.CodecResult.1", "fieldStatus": "NOT_RUN", "normalDemodObservations": 93,
        "exactNonClockReplayMatch": True, "exactPacketMatch": True, "exactContainerMatch": identity(left/"channel.mkv")["sha256"] == identity(right/"channel.mkv")["sha256"],
        "trials": results, "fullStep3Status": "PARTIAL"})
