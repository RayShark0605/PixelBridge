from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import subprocess
import sys

import blake3
import json

from build_codec_corpus import (
    CASES,
    CorpusError,
    MAX_TRANSPORT_BLOCK_BYTES,
    SOURCE_FRAMES,
    SOURCE_FIRST_FRAME_SEQUENCE,
    SOURCE_HEIGHT,
    SOURCE_LAYOUT_VERSION,
    SOURCE_PROFILE,
    SOURCE_PROFILE_ID,
    SOURCE_SESSION_TAG,
    SOURCE_WIDTH,
    build_corpus,
    canonical_json,
    codec_arguments,
    decode_arguments,
    normalize_probe_document,
    normalize_cli_json,
    normalize_text_output,
    normalized_command,
    parse_json_bytes,
    run_bounded,
    validate_canonical_report,
    validate_evaluation_input,
    validate_evaluation_report,
    validate_probe,
    validate_source_report,
)


def make_probe(case: dict[str, object]) -> dict[str, object]:
    frames = []
    for index in range(SOURCE_FRAMES):
        frames.append({
            "media_type": "video",
            "key_frame": 1 if case["intraOnly"] or index == 0 else 0,
            "pict_type": "I" if case["intraOnly"] or index == 0 else "P",
            "color_range": case["colorRange"],
            "color_space": "bt709",
            "color_transfer": "bt709",
            "color_primaries": "bt709",
        })
    return {
        "streams": [{
            "codec_type": "video",
            "codec_name": case["codec"],
            "pix_fmt": case.get("inspectedPixelFormat", case["pixelFormat"]),
            "width": SOURCE_WIDTH,
            "height": SOURCE_HEIGHT,
            "color_space": "bt709",
            "color_range": case["colorRange"],
            "color_transfer": "bt709",
            "color_primaries": "bt709",
            "profile": "fixture",
            "level": 1,
        }],
        "frames": frames,
        "packets": [{"codec_type": "video"} for _ in range(SOURCE_FRAMES)],
    }


class BuildCodecCorpusTests(unittest.TestCase):
    def test_canonical_json_and_parser_are_strict_and_deterministic(self) -> None:
        first = canonical_json({"z": 2, "a": [1, "x"]})
        second = canonical_json({"a": [1, "x"], "z": 2})
        self.assertEqual(first, second)
        self.assertEqual(first, b'{"a":[1,"x"],"z":2}\n')
        self.assertEqual(parse_json_bytes(first, "fixture"), {"a": [1, "x"], "z": 2})

        with self.assertRaisesRegex(CorpusError, "duplicate JSON member"):
            parse_json_bytes(b'{"outer":{"x":1,"x":2}}', "duplicate")
        with self.assertRaisesRegex(CorpusError, "non-finite JSON number"):
            parse_json_bytes(b'{"value":NaN}', "nonfinite")
        with self.assertRaisesRegex(CorpusError, "must be a JSON object"):
            parse_json_bytes(b'[]', "array")
        with self.assertRaisesRegex(CorpusError, "valid UTF-8 JSON"):
            parse_json_bytes(b'\xff', "invalid-utf8")
        self.assertEqual(normalize_cli_json(b"{}\r\n", "fixture"), b"{}\n")
        with self.assertRaisesRegex(CorpusError, "carriage return"):
            normalize_cli_json(b'{"value":"raw\rvalue"}\r\n', "fixture")
        self.assertEqual(normalize_text_output(b"line1\r\nline2\r\n", "fixture"), b"line1\nline2\n")
        with self.assertRaisesRegex(CorpusError, "control byte"):
            normalize_text_output(b"line\rvalue", "fixture")

    def test_probe_validation_checks_codec_geometry_color_and_temporal_shape(self) -> None:
        for case in CASES:
            inspection = validate_probe(make_probe(case), case)
            self.assertEqual(inspection["codecName"], case["codec"])
            self.assertEqual(inspection["pixelFormat"], case.get("inspectedPixelFormat", case["pixelFormat"]))
            self.assertEqual(inspection["frameCount"], SOURCE_FRAMES)
            self.assertEqual(inspection["packetCount"], SOURCE_FRAMES)

        combined_case = CASES[0]
        combined_probe = make_probe(combined_case)
        combined_probe["packets_and_frames"] = []
        for packet, frame in zip(combined_probe.pop("packets"), combined_probe.pop("frames"), strict=True):
            combined_probe["packets_and_frames"].append({"type": "packet", **packet})
            combined_probe["packets_and_frames"].append({"type": "frame", **frame})
        combined_inspection = validate_probe(combined_probe, combined_case)
        self.assertEqual(combined_inspection["frameCount"], SOURCE_FRAMES)
        self.assertEqual(combined_inspection["packetCount"], SOURCE_FRAMES)

        case = CASES[1]
        wrong_codec = make_probe(case)
        wrong_codec["streams"][0]["codec_name"] = "hevc"
        with self.assertRaisesRegex(CorpusError, "codec mismatch"):
            validate_probe(wrong_codec, case)

        wrong_color = make_probe(case)
        wrong_color["streams"][0]["color_space"] = "smpte170m"
        with self.assertRaisesRegex(CorpusError, "color metadata mismatch"):
            validate_probe(wrong_color, case)

        wrong_frame_color = make_probe(case)
        wrong_frame_color["frames"][1]["color_transfer"] = "unknown"
        with self.assertRaisesRegex(CorpusError, "per-frame color metadata mismatch"):
            validate_probe(wrong_frame_color, case)

        missing_packet = make_probe(case)
        missing_packet["packets"].pop()
        with self.assertRaisesRegex(CorpusError, "frame/packet count mismatch"):
            validate_probe(missing_packet, case)

        no_keyframe = make_probe(case)
        for frame in no_keyframe["frames"]:
            frame["key_frame"] = 0
        with self.assertRaisesRegex(CorpusError, "exactly one key frame"):
            validate_probe(no_keyframe, case)

        extra_stream = make_probe(case)
        extra_stream["streams"].append({"codec_type": "audio"})
        with self.assertRaisesRegex(CorpusError, "exactly one stream"):
            validate_probe(extra_stream, case)

        audio_packet = make_probe(case)
        audio_packet["packets"].append({"codec_type": "audio"})
        with self.assertRaisesRegex(CorpusError, "non-video frame or packet"):
            validate_probe(audio_packet, case)

        mixed_shape = make_probe(case)
        mixed_shape["packets_and_frames"] = []
        with self.assertRaisesRegex(CorpusError, "mixes combined and separate"):
            validate_probe(mixed_shape, case)

        malformed_streams = make_probe(case)
        malformed_streams["streams"] = {"codec_type": "video"}
        with self.assertRaisesRegex(CorpusError, "array of objects"):
            validate_probe(malformed_streams, case)

    def test_codec_reports_require_canonical_seals_and_consistent_frame_summaries(self) -> None:
        payload = {"value": 1}
        payload_bytes = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        report = {
            "schema": "fixture",
            "version": 1,
            "payloadBlake3": blake3.blake3(payload_bytes).hexdigest(),
            "payload": payload,
        }
        contents = (json.dumps(report, separators=(",", ":")) + "\n").encode("utf-8")
        self.assertEqual(validate_canonical_report(contents, "fixture", "fixture"), report)
        corrupted = contents.replace(report["payloadBlake3"].encode("ascii"), b"0" * 64)
        with self.assertRaisesRegex(CorpusError, "payloadBlake3 mismatch"):
            validate_canonical_report(corrupted, "fixture", "fixture")
        report["version"] = 1.0
        floating_version = (json.dumps(report, separators=(",", ":")) + "\n").encode("utf-8")
        with self.assertRaisesRegex(CorpusError, "schema/version/payload mismatch"):
            validate_canonical_report(floating_version, "fixture", "fixture")

        frames = []
        for index, classification in enumerate(("Verified", "ErasureNoFalseAccept", "RejectedNoFalseAccept")):
            accepted_blocks = [
                {"slot": slot, "byteCount": 36, "blake3": blake3.blake3(f"block-{slot}".encode()).hexdigest()}
                for slot in range(4)
            ] if index == 0 else []
            accepted = 4 if index == 0 else 0
            failures = 0 if index == 0 else 4
            diagnostic_evaluation = {
                "evaluated": True,
                "paddingValid": True,
                "codewords": 4,
                "fecFailures": failures,
                "crcFailures": 0,
                "identityFailures": 0,
                "falseAcceptedCodewords": 0,
                "acceptedTransportBlocks": accepted,
                "acceptedRemoteControlBlocks": 0,
                "iterationsTotal": 0,
                "iterationsMaximum": 0,
                "comparedCodedBits": 64800,
                "erroneousCodedBits": 0,
            }
            production_evaluation = dict(diagnostic_evaluation)
            production_evaluation["comparedCodedBits"] = 0
            frames.append({
                "index": index,
                "classification": classification,
                "diagnosticFalseCandidates": 0,
                "falseAcceptedCodewords": 0,
                "diagnosticTruthEvaluation": diagnostic_evaluation,
                "productionTransportEvaluation": production_evaluation,
                "productionAcceptedBlocks": accepted_blocks,
            })
        evaluation_report = {"payload": {
            "frames": frames,
            "receiverEvidence": {
                "configuredSegments": 3,
                "inputTransportBlocks": 4,
                "parsedTransportBlocks": 4,
                "uniqueOuterSymbols": 4,
                "identicalDuplicateOuterSymbols": 0,
                "recoveryReadyOuterSymbols": 0,
                "alreadyCompletedOuterSymbols": 0,
                "receiverRejections": 0,
                "outerConflictRejections": 0,
                "resourcePolicyRejections": 0,
                "verifiedSegments": 1,
                "verifiedRawBytes": 5256,
                "finalizationPrepared": False,
                "wholeFileDigestDisposition": "NotReady",
                "expectedWholeFileBlake3": "1" * 64,
                "observedWholeFileBlake3": None,
                "safe": True,
            },
            "summary": {
                "frameCount": 3,
                "verifiedFrames": 1,
                "erasureFrames": 1,
                "rejectedFrames": 1,
                "diagnosticAcceptedTransportBlocks": 4,
                "productionAcceptedTransportBlocks": 4,
                "falseAcceptedCodewords": 0,
                "diagnosticFalseCandidates": 0,
                "truthBoundaryValid": True,
                "allFramesVerified": False,
            },
        }}
        self.assertEqual(validate_evaluation_report(evaluation_report, "fixture")["verifiedFrames"], 1)
        evaluation_report["payload"]["summary"]["productionAcceptedTransportBlocks"] = 5
        with self.assertRaisesRegex(CorpusError, "summary consistency"):
            validate_evaluation_report(evaluation_report, "fixture")

        evaluation_report["payload"]["summary"]["productionAcceptedTransportBlocks"] = 4
        evaluation_report["payload"]["frames"][0]["productionAcceptedBlocks"][0]["byteCount"] = \
            MAX_TRANSPORT_BLOCK_BYTES + 1
        with self.assertRaisesRegex(CorpusError, "violates bounds"):
            validate_evaluation_report(evaluation_report, "fixture")

    def test_source_and_evaluation_reports_bind_exact_raw_bytes(self) -> None:
        source_frame_bytes = SOURCE_WIDTH * SOURCE_HEIGHT * 4
        source_bytes = b"\x11" * (source_frame_bytes * SOURCE_FRAMES)
        source_frames = []
        for index in range(SOURCE_FRAMES):
            start = index * source_frame_bytes
            source_frames.append({
                "index": index,
                "frameSequence": SOURCE_FIRST_FRAME_SEQUENCE + index,
                "blake3": blake3.blake3(source_bytes[start:start + source_frame_bytes]).hexdigest(),
            })
        source_report = {"payload": {
            "format": "bgra8",
            "width": SOURCE_WIDTH,
            "height": SOURCE_HEIGHT,
            "frameBytes": source_frame_bytes,
            "frameCount": SOURCE_FRAMES,
            "profile": SOURCE_PROFILE,
            "profileId": SOURCE_PROFILE_ID,
            "layoutVersion": SOURCE_LAYOUT_VERSION,
            "sessionTag": SOURCE_SESSION_TAG,
            "sequenceBlake3": blake3.blake3(source_bytes).hexdigest(),
            "frames": source_frames,
        }}

        gray_frame_bytes = SOURCE_WIDTH * SOURCE_HEIGHT
        decoded_bytes = b"\x22" * (gray_frame_bytes * SOURCE_FRAMES)
        evaluation_frames = []
        for index in range(SOURCE_FRAMES):
            start = index * gray_frame_bytes
            evaluation_frames.append({
                "index": index,
                "inputBlake3": blake3.blake3(decoded_bytes[start:start + gray_frame_bytes]).hexdigest(),
            })
        evaluation_report = {"payload": {
            "input": {
                "format": "gray8",
                "width": SOURCE_WIDTH,
                "height": SOURCE_HEIGHT,
                "frameBytes": gray_frame_bytes,
                "frameCount": SOURCE_FRAMES,
                "byteCount": len(decoded_bytes),
                "blake3": blake3.blake3(decoded_bytes).hexdigest(),
            },
            "frames": evaluation_frames,
        }}

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_path = root / "source.bgra"
            source_path.write_bytes(source_bytes)
            validate_source_report(source_report, source_path)
            source_report["payload"]["layoutVersion"] = float(SOURCE_LAYOUT_VERSION)
            with self.assertRaisesRegex(CorpusError, "nonnegative integer"):
                validate_source_report(source_report, source_path)
            source_report["payload"]["layoutVersion"] = SOURCE_LAYOUT_VERSION
            source_report["payload"]["frames"][1]["blake3"] = "0" * 64
            with self.assertRaisesRegex(CorpusError, "does not bind"):
                validate_source_report(source_report, source_path)

            decoded_path = root / "decoded.gray"
            decoded_path.write_bytes(decoded_bytes)
            validate_evaluation_input(evaluation_report, decoded_path, "fixture")
            evaluation_report["payload"]["input"]["frameCount"] = float(SOURCE_FRAMES)
            with self.assertRaisesRegex(CorpusError, "nonnegative integer"):
                validate_evaluation_input(evaluation_report, decoded_path, "fixture")
            evaluation_report["payload"]["input"]["frameCount"] = SOURCE_FRAMES
            evaluation_report["payload"]["input"]["blake3"] = "0" * 64
            with self.assertRaisesRegex(CorpusError, "does not bind"):
                validate_evaluation_input(evaluation_report, decoded_path, "fixture")

    def test_codec_commands_are_explicit_create_only_and_path_normalized(self) -> None:
        ffmpeg = Path("C:/fixture/bin/ffmpeg.exe")
        source = Path("C:/fixture/corpus/source.bgra")
        output = Path("C:/fixture/corpus/case/encoded.mkv")
        encode = codec_arguments(ffmpeg, source, output, CASES[0])
        decode = decode_arguments(ffmpeg, output, Path("C:/fixture/corpus/case/decoded.gray"))
        self.assertIn("-n", encode)
        self.assertNotIn("-y", encode)
        self.assertIn("-n", decode)
        self.assertNotIn("-y", decode)
        self.assertEqual(encode[encode.index("-frames:v") + 1], str(SOURCE_FRAMES))
        self.assertEqual(decode[decode.index("-frames:v") + 1], str(SOURCE_FRAMES))
        self.assertIn("-fs", encode)
        self.assertIn("-fs", decode)

        normalized = normalized_command(
            encode,
            {"ffmpeg": ffmpeg},
            Path("C:/fixture/corpus"),
        )
        self.assertEqual(normalized[0], "<ffmpeg>")
        self.assertIn("<corpus-root>/source.bgra", normalized)
        self.assertIn("<corpus-root>/case/encoded.mkv", normalized)

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            bitstream = root / "case" / "encoded.mkv"
            bitstream.parent.mkdir()
            bitstream.write_bytes(b"fixture")
            probe = {"format": {"filename": str(bitstream)}}
            normalize_probe_document(probe, bitstream, root)
            self.assertEqual(probe["format"]["filename"], "case/encoded.mkv")

            other = root / "other.mkv"
            other.write_bytes(b"other")
            wrong_probe = {"format": {"filename": str(other)}}
            with self.assertRaisesRegex(CorpusError, "unexpected file"):
                normalize_probe_document(wrong_probe, bitstream, root)

    def test_subprocess_capture_is_bounded_and_removes_failed_file_output(self) -> None:
        exact = run_bounded([sys.executable, "-c", "import sys;sys.stdout.buffer.write(b'x'*32)"],
                            maximum_stdout_bytes=32, maximum_stderr_bytes=32, timeout_seconds=5)
        self.assertEqual(exact, b"x" * 32)

        with self.assertRaisesRegex(CorpusError, "stdout exceeds 32 bytes"):
            run_bounded([sys.executable, "-c", "import sys;sys.stdout.buffer.write(b'x'*33)"],
                        maximum_stdout_bytes=32, maximum_stderr_bytes=32, timeout_seconds=5)
        with self.assertRaisesRegex(CorpusError, "stderr exceeds 32 bytes"):
            run_bounded([sys.executable, "-c", "import sys;sys.stderr.buffer.write(b'x'*33)"],
                        maximum_stdout_bytes=32, maximum_stderr_bytes=32, timeout_seconds=5)
        with self.assertRaisesRegex(CorpusError, "subprocess failed with exit 7"):
            run_bounded([sys.executable, "-c", "import sys;sys.stderr.write('fixture');sys.exit(7)"],
                        maximum_stdout_bytes=32, maximum_stderr_bytes=32, timeout_seconds=5)
        with self.assertRaises(subprocess.TimeoutExpired):
            run_bounded([sys.executable, "-c", "import time;time.sleep(2)"],
                        maximum_stdout_bytes=32, maximum_stderr_bytes=32, timeout_seconds=0.05)

        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / "bounded.bin"
            run_bounded([sys.executable, "-c", "import sys;sys.stdout.buffer.write(b'y'*32)"],
                        stdout_path=output, maximum_stdout_bytes=32, maximum_stderr_bytes=32, timeout_seconds=5)
            self.assertEqual(output.read_bytes(), b"y" * 32)
            overflow = Path(temporary_directory) / "overflow.bin"
            with self.assertRaisesRegex(CorpusError, "stdout exceeds 32 bytes"):
                run_bounded([sys.executable, "-c", "import sys;sys.stdout.buffer.write(b'z'*33)"],
                            stdout_path=overflow, maximum_stdout_bytes=32, maximum_stderr_bytes=32,
                            timeout_seconds=5)
            self.assertFalse(overflow.exists())

    def test_existing_output_directory_is_rejected_before_process_execution(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            executable = root / "fixture.exe"
            executable.write_bytes(b"fixture")
            output = root / "existing"
            output.mkdir()
            sentinel = output / "sentinel.txt"
            sentinel.write_text("unchanged", encoding="utf-8")

            with self.assertRaises(FileExistsError):
                build_corpus(executable, executable, executable, output)
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "unchanged")
            self.assertEqual(sorted(path.name for path in output.iterdir()), ["sentinel.txt"])


if __name__ == "__main__":
    unittest.main()
