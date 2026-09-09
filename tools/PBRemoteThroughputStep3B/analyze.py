"""Recheck sealed inputs and existing traces without invoking Encoder/Decoder."""
from __future__ import annotations
import argparse
from collections import Counter
from pathlib import Path
from codec import inspect_data, encode_arguments
from run import (PARAMETERS, MIB, require, local, read_bytes, read_json, write_json, identity,
                 fixture, validate_ledger, rows, analyze_replay)


def non_clock_receiver(summary):
    report = summary["receiverReport"]
    counters = report["stageCounters"]
    return {"state": report["state"], "profile": report["profile"], "recovery": report["recovery"], "publish": {key: value for key,value in report["publish"].items() if key != "finalPath"},
        "counters": {key: value for key, value in counters.items() if not key.endswith("TimeTotal100ns")},
        "stageCalls": {key: {name: value[name] for name in ("calls", "samples", "unavailableReason")} for key, value in summary["diagnostics"]["stages"].items()}}


def stages(trace):
    bootstrap = sum(bool(frame["bootstrapAccepted"]) for frame in trace)
    attempted = [frame for frame in trace if frame["kind"] == 4 and frame["geometry"] == 1 and frame["frameErasure"] == 0]
    slots = [slot for frame in attempted for slot in frame["slots"]]
    return {"bootstrapAccepted": bootstrap, "bootstrapRejected": len(trace)-bootstrap,
        "bootstrapErasureHistogram": dict(Counter(str(frame["bootstrapErasure"]) for frame in trace)),
        "frameErasureHistogram": dict(Counter(str(frame["frameErasure"]) for frame in trace)),
        "doubleBootstrapCopiesValid": sum(all(all(copy[index] for index in (0, 1, 2)) for copy in frame["bootstrapCopies"]) for frame in trace),
        "reachedDataDemodObservations": len(attempted),
        "freshnessCurrentCount": sum(sum(frame["freshnessCurrent"]) for frame in attempted) if attempted else None,
        "fecAcceptedSlots": sum(slot[3] for slot in slots) if slots else None,
        "crcAcceptedSlots": sum(slot[4] for slot in slots) if slots else None,
        "acceptedPayloadDigestComparisons": sum(frame["acceptedBlocks"] for frame in trace),
        "preFecBer": None, "unreachedStageReason": None if attempted else "RejectedBeforeDataDemod_NotZeroErrorEvidence"}


def analyze(root, output):
    require(not output.exists(), "Analysis output is create-only")
    require(read_json(root / "context/parameters-v2.json") == PARAMETERS, "Parameter identity changed")
    fixture(root)
    validate_ledger(read_json(root / "source/ledger.json"))
    require(identity(root / "frozen-pixels/source.bgra") == read_json(root / "context/frozen-input.json"), "Frozen input hash changed")
    for item in read_json(root / "context/codec-runtime.json")["files"]:
        require(identity(Path(item["frozen"]["path"])) == item["frozen"], "Pinned codec runtime hash changed")
    raw = analyze_replay(root, root / "raw-full", raw=True)
    require(raw["recovered"], "Raw multi-frame proof failed")
    prefix = read_json(root / "raw-prefix/summary.json")
    prefix_rows = rows(root / "raw-prefix/frames.jsonl")
    require(prefix["frames"] == len(prefix_rows) == 3 and prefix["intentionalPrefix"] and prefix["prefixLimitReached"] and not prefix["error"] and not prefix["publishedAndReopened"], "Prefix proof changed")
    require(all(frame["bootstrapAccepted"] for frame in prefix_rows) and prefix["receiverReport"]["stageCounters"]["outerUniqueSymbols"] > 0, "Prefix must reach bound production Outer, not fail only on missing control")
    require(not any(prefix["receiverReport"]["publish"][key] for key in ("wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published")), "Prefix publication flags changed")
    truth = rows(root / "raw-full/frames.jsonl")
    require(len({(row["sessionTag"], row["frameSequence"]) for row in truth}) == 30, "Thirty actual distinct raw identities required")
    trials, signatures, packets, extra_data = [], [], [], []
    for number in (1, 2):
        name = f"codec-v2-{number:02}"
        trial = root / name
        before = identity(trial / "channel.mkv", 16*MIB)
        require(before == read_json(trial / "bitstream-identity.json"), "Codec artifact changed")
        metadata = read_json(root / "logs" / f"{name}-packets.log")
        pictures = read_json(root / "logs" / f"{name}-pictures.log")
        require(Path(metadata["format"]["filename"]).resolve() == (trial/"channel.mkv").resolve() and int(metadata["format"]["size"]) == before["bytes"], "Probe and actual bitstream identity mismatch")
        encode_process = read_json(root / "logs" / f"{name}-encode.process.json")
        require(encode_process["argv"] == list(map(str,encode_arguments(root/"codec-runtime/ffmpeg.exe",root/"frozen-pixels/source.bgra",trial/"channel.mkv"))) and
                encode_process["returnCode"] == 0 and not encode_process["failures"] and encode_process["jobAssignedBeforeResume"], "Encoding command or process authority mismatch")
        inspection = inspect_data(metadata, pictures,
            read_bytes(root / "logs" / f"{name}-headers.log", 8*MIB).decode("utf-8"),
            read_bytes(root / "logs" / f"{name}-encode.log", MIB).decode("utf-8"))
        require(inspection == read_json(trial / "inspection.json"), "Inspection report changed")
        recovery = analyze_replay(root, trial / "replay", truth=truth)
        require(recovery == read_json(trial / "recovery-analysis.json"), "Recovery analysis changed")
        trace, pixels = rows(trial / "replay/frames.jsonl"), rows(trial / "replay/pixels.jsonl")
        summary = read_json(trial / "replay/summary.json")
        require(summary["media"]["sourceBlake3"], "Missing actual media provenance")
        from blake3 import blake3
        require(summary["media"]["sourceBlake3"] == blake3(read_bytes(trial / "channel.mkv", 16*MIB)).hexdigest(), "Media decoder read different bitstream")
        signatures.append({"frames": [{key: value for key, value in row.items() if key != "processingQpc100ns"} for row in trace],
            "pixels": pixels, "receiver": non_clock_receiver(summary)})
        packets.append(metadata["packets"])
        extra_data.append(metadata["streams"][0]["extradata_hash"])
        trials.append({"inspection": inspection, "recovery": recovery, "stages": stages(trace)})
        require(identity(trial / "channel.mkv", 16*MIB) == before, "Codec artifact mutated during analysis")
    require(signatures[0] == signatures[1], "Non-clock frame/pixel/Receiver/stage-call repeatability failed")
    require(packets[0] == packets[1] and extra_data[0] == extra_data[1], "Encoded access units or parameter-set bytes differ")
    log_records = [read_json(path) for path in sorted((root/"logs").glob("*.process.json"))]
    require(len(log_records) <= 128 and sum(row["processingSeconds"] for row in log_records) <= 600, "Process time budget exceeded")
    result = {"schema": "PixelBridge.Step3B.Analysis.1", "localStatus": "FIXED_CODEC_DIAGNOSTIC_COMPLETE",
        "rawMultiFrameRecovery": "PASS", "codecRecovery": "PASS" if all(trial["recovery"]["recovered"] for trial in trials) else "NOT_RECOVERED",
        "fullStep3Status": "PARTIAL", "fieldStatus": "NOT_RUN", "sourceBytes": 65536, "encodedBytes": 65536,
        "prefixFrames": 3, "prefixOuterUniqueSymbols": prefix["receiverReport"]["stageCounters"]["outerUniqueSymbols"],
        "normalDemodObservations": 93, "normalDemodObservationBasis": "3 prefix + 30 raw + 30 per codec trial",
        "original": raw, "originalStages": stages(truth), "trials": trials,
        "exactNonClockPixelFrameReceiverMatch": True, "exactAccessUnitsAndParameterSetsMatch": True,
        "exactContainerMatch": identity(root/"codec-v2-01/channel.mkv")["sha256"] == identity(root/"codec-v2-02/channel.mkv")["sha256"],
        "normalAndGuardProcessSecondsSoFar": sum(row["processingSeconds"] for row in log_records),
        "peakObservedJobCommitBytes": max(row.get("peakJobCommitBytes",0) for row in log_records),
        "acceptedPayloadOracleNote": "Same-ordinal raw payload digest comparisons only where codec accepted payload exists; zero comparisons are not a false-acceptance certification",
        "modelNote": "Synthetic development sequence; not calibrated Citrix behavior or Holdout; geometry gate unchanged",
        "liveChannelGoodput": None, "simulatedVerifiedGoodput": None, "newScreenOrRemoteTests": False,
        "mediaTimingNote": "2 second nominal duration; actual container PTS preserved; first-last span is 1.933 s; no channel throughput inference"}
    write_json(output, result)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=local)
    parser.add_argument("--output", required=True, type=local)
    args = parser.parse_args()
    print(analyze(args.root, args.output)["codecRecovery"])
