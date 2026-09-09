"""Scoped negative checks. Media-only processes never create WARP/Receiver."""
from __future__ import annotations
import argparse
import copy
import json
import shutil
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch
import run
from codec import inspect_data, encode_arguments, environment
from process_runner import invoke

ROOT = OUTPUT = EXE = BAD_COLOR_MEDIA = None


class Guards(unittest.TestCase):
    def setUp(self):
        self.directory = OUTPUT / self._testMethodName
        self.directory.mkdir()
        self.metadata = run.read_json(ROOT / "logs/codec-v2-01-packets.log")
        self.pictures = run.read_json(ROOT / "logs/codec-v2-01-pictures.log")
        self.headers = run.read_bytes(ROOT / "logs/codec-v2-01-headers.log", 8*run.MIB).decode("utf-8")
        self.encoding = run.read_bytes(ROOT / "logs/codec-v2-01-encode.log", run.MIB).decode("utf-8")

    def inspect(self):
        return inspect_data(self.metadata, self.pictures, self.headers, self.encoding)

    def rejected_inspection(self):
        run.write_json(self.directory / "metadata.json", self.metadata)
        run.write_json(self.directory / "pictures.json", self.pictures)
        with self.assertRaises(ValueError):
            self.inspect()

    def child(self, argv, expected_failure=True, *, timeout=10, limit=run.MIB, output_limits=(), env=None):
        spent = sum(run.read_json(path)["processingSeconds"] for path in (ROOT/"logs").glob("*.process.json"))
        self.assertLess(spent, 600)
        record = invoke(argv, ROOT / "logs" / f"guard-{OUTPUT.name}-{self._testMethodName}.log", min(timeout,600-spent), limit=limit, output_limits=output_limits, env=env)
        self.assertEqual(bool(record["failures"] or record.get("returnCode") != 0), expected_failure)
        return record

    def test_valid_inspection(self):
        self.assertTrue(self.inspect()["passed"])

    def test_source_generator(self):
        value = run.source_bytes()
        self.assertEqual(len(value),65536)
        self.assertEqual(run.blake3(value).hexdigest(), "d38ef0993b8f097aee9ad93267387bdef83c49e1f0c54e35be8c87d23974aebf")
        run.validate_ledger(run.read_json(ROOT/"source/ledger.json"))

    def test_compressed_or_incomplete_ledger(self):
        for key, value in (("complete", False), ("encodedBytes",280)):
            ledger = run.read_json(ROOT/"source/ledger.json")
            ledger[key] = value
            with self.assertRaises(ValueError):
                run.validate_ledger(ledger)

    def test_duplicate_json_and_nan(self):
        for text in ('{"key":1,"key":2}', '{"key":NaN}', '{"key":1e999}'):
            with self.assertRaises(ValueError):
                run.parse(text)

    def test_json_limit(self):
        path = self.directory/"large.json"
        with path.open("xb") as stream:
            stream.write(b" "*(run.MIB+1))
        with self.assertRaises(ValueError):
            run.read_json(path)

    def test_existing_output(self):
        path=self.directory/"existing.json"
        run.write_json(path,{"preserved":True})
        before=run.identity(path)
        with self.assertRaises(FileExistsError):
            run.write_json(path,{"preserved":False})
        self.assertEqual(before,run.identity(path))

    def test_frame_count(self):
        self.pictures["frames"].pop()
        self.rejected_inspection()

    def test_pts(self):
        self.metadata["packets"][1]["pts"] += 1
        self.rejected_inspection()

    def test_dts(self):
        self.metadata["packets"][1]["dts"] -= 1
        self.rejected_inspection()

    def test_duration(self):
        self.pictures["frames"][0]["duration"] = 0
        self.rejected_inspection()

    def test_color_missing(self):
        del self.metadata["streams"][0]["color_transfer"]
        self.rejected_inspection()

    def test_frame_color(self):
        self.pictures["frames"][0]["color_range"] = "pc"
        self.rejected_inspection()

    def test_multiple_streams(self):
        self.metadata["streams"].append({"codec_type":"audio"})
        self.rejected_inspection()

    def test_idr(self):
        self.metadata["packets"][15]["flags"] = "___"
        self.rejected_inspection()

    def test_all_intra(self):
        self.pictures["frames"][1]["pict_type"] = "I"
        self.rejected_inspection()

    def test_b_frame(self):
        self.pictures["frames"][1]["pict_type"] = "B"
        self.rejected_inspection()

    def test_hrd_missing(self):
        self.headers = self.headers.replace("nal_hrd_parameters_present_flag", "removed_hrd_present_flag")
        with self.assertRaises(ValueError):
            self.inspect()

    def test_vbv_warning(self):
        self.encoding += "\nVBV underflow"
        with self.assertRaises(ValueError):
            self.inspect()

    def test_packet_envelope(self):
        self.metadata["packets"][0]["size"] = str(2*run.MIB)
        self.rejected_inspection()

    def test_missing_hash(self):
        del self.metadata["packets"][0]["data_hash"]
        self.rejected_inspection()

    def test_process_launch_failure(self):
        self.child([self.directory/"missing-codec.exe"])

    def test_process_failure(self):
        self.child([sys.executable,"-B","-c","raise SystemExit(7)"])

    def test_process_timeout(self):
        result=self.child([sys.executable,"-B","-c","import time; time.sleep(60)"],timeout=0.2)
        self.assertIn("Process timeout",result["failures"])

    def test_bounded_log(self):
        self.child([sys.executable,"-B","-c","print('x'*4096)"],limit=128)
        self.assertEqual((ROOT/"logs"/f"guard-{OUTPUT.name}-{self._testMethodName}.log").stat().st_size,128)

    def test_bounded_output(self):
        path=self.directory/"oversize.bin"
        code=f"from pathlib import Path; import time; Path({str(path)!r}).write_bytes(b'x'*128); time.sleep(1)"
        result=self.child([sys.executable,"-B","-c",code],output_limits=[(path,64)])
        self.assertTrue(any("Output byte limit" in message for message in result["failures"]))

    def test_missing_encoder(self):
        ffmpeg=ROOT/"codec-runtime/ffmpeg.exe"
        argv=encode_arguments(ffmpeg,ROOT/"frozen-pixels/source.bgra",self.directory/"invalid.mkv")
        argv[argv.index("libx264")]="step3b_missing_encoder"
        self.child(argv,env=environment(ffmpeg.parent))

    def test_real_bad_header(self):
        media=self.directory/"bad-header.mkv"
        with media.open("xb") as stream:
            stream.write(b"Step3B invalid Matroska header"*4)
        self.child([EXE,"--media-check",media,self.directory/"check"])
        self.assertFalse((self.directory/"check/output").exists())

    def test_real_truncated_tail(self):
        original=ROOT/"codec-v2-01/channel.mkv"
        packet=self.metadata["packets"][-1]
        stop=int(packet["pos"])+int(packet["size"])-128
        data=run.read_bytes(original,16*run.MIB)
        self.assertTrue(0<stop<len(data))
        media=self.directory/"truncated-tail.mkv"
        with media.open("xb") as stream:
            stream.write(data[:stop])
        self.child([EXE,"--media-check",media,self.directory/"check"])
        summary=run.read_json(self.directory/"check/summary.json")
        self.assertTrue(summary["error"])
        self.assertFalse(summary["reachedEof"])
        self.assertFalse((self.directory/"check/output").exists())

    def test_real_unspecified_color(self):
        # The preserved real first attempt, not a invented JSON-only assertion.
        self.child([EXE,"--media-check",BAD_COLOR_MEDIA,self.directory/"check"])
        summary=run.read_json(self.directory/"check/summary.json")
        self.assertIn("color contract",summary["error"])
        self.assertEqual(summary["frames"],0)

    def test_existing_replay_directory(self):
        target=self.directory/"existing"
        target.mkdir()
        sentinel=target/"sentinel.txt"
        sentinel.write_text("preserve",encoding="utf-8")
        before=run.identity(sentinel)
        self.child([EXE,"--media-check",ROOT/"codec-v2-01/channel.mkv",target])
        self.assertEqual(before,run.identity(sentinel))

    def test_tree_budget_and_depth(self):
        path=self.directory/"tree"
        path.mkdir()
        (path/"item").write_bytes(b"1234")
        with self.assertRaises(ValueError):
            run.tree_identity(path,byte_limit=3)
        for _ in range(17):
            path=path/"child"
            path.mkdir()
        with self.assertRaises(ValueError):
            run.tree_identity(self.directory)

    def test_final_publication_gate_tamper(self):
        target=self.directory/"replay"
        target.mkdir()
        for name in ("frames.jsonl","pixels.jsonl"):
            shutil.copyfile(ROOT/"raw-full"/name,target/name)
        summary=run.read_json(ROOT/"raw-full/summary.json")
        summary["receiverReport"]["publish"]["wholeDigestVerified"]=False
        run.write_json(target/"summary.json",summary)
        with self.assertRaisesRegex(ValueError,"Publication gate incomplete"):
            run.analyze_replay(ROOT,target,raw=True)

    def test_false_accepted_payload(self):
        target=self.directory/"replay"
        target.mkdir()
        shutil.copyfile(ROOT/"codec-v2-01/replay/pixels.jsonl",target/"pixels.jsonl")
        shutil.copyfile(ROOT/"codec-v2-01/replay/summary.json",target/"summary.json")
        trace=run.rows(ROOT/"codec-v2-01/replay/frames.jsonl")
        truth=run.rows(ROOT/"raw-full/frames.jsonl")
        row=trace[0]
        record=copy.deepcopy(truth[0]["acceptedPayloadDigests"][0])
        record[-1]="0"*64
        row["sessionTag"],row["frameSequence"]=truth[0]["sessionTag"],truth[0]["frameSequence"]
        row["acceptedBlocks"]=1
        row["acceptedPayloadDigests"]=[record]
        row["slots"][record[0]][5]=1
        with (target/"frames.jsonl").open('x',encoding='utf-8') as stream:
            for frame in trace:stream.write(json.dumps(frame)+'\n')
        with self.assertRaisesRegex(ValueError,"False accepted payload"):
            run.analyze_replay(ROOT,target,truth=truth)

    def test_unreached_stages_null(self):
        from analyze import stages
        result=stages(run.rows(ROOT/"codec-v2-01/replay/frames.jsonl"))
        self.assertEqual(result['reachedDataDemodObservations'],0)
        for key in ('freshnessCurrentCount','fecAcceptedSlots','crcAcceptedSlots','preFecBer'):
            self.assertIsNone(result[key])


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--root",required=True,type=run.local)
    parser.add_argument("--output",required=True,type=run.local)
    parser.add_argument("--exe",required=True,type=run.local)
    parser.add_argument("--tests",nargs="+",help="Only explicitly selected existing guards; no runtime matrix")
    parser.add_argument("--bad-color-media",type=run.local,help="Explicit preserved real unspecified-color fixture, needed for a fresh-run full guard suite")
    args=parser.parse_args()
    ROOT,OUTPUT,EXE=args.root,args.output,args.exe
    BAD_COLOR_MEDIA=args.bad_color_media or ROOT/'codec-01/channel.mkv'
    OUTPUT.mkdir()
    before={name:run.identity(ROOT/name) for name in ("frozen-pixels/source.bgra","codec-v2-01/channel.mkv","codec-v2-02/channel.mkv")}
    started=time.monotonic()
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(Guards) if not args.tests else unittest.TestSuite(Guards(name) for name in args.tests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    run.require(before=={name:run.identity(ROOT/name) for name in before},"Guard changed primary input")
    run.write_json(OUTPUT/"RESULT.json",{"schema":"PixelBridge.Step3B.Guards.1","tests":result.testsRun,"failures":len(result.failures),"errors":len(result.errors),
        "passed":result.wasSuccessful(),"seconds":time.monotonic()-started,"newDemodObservations":0,"primaryInputsUnchanged":True})
    raise SystemExit(0 if result.wasSuccessful() else 1)
