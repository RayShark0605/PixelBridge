"""Independent file, unchanged-trace and nullable-decision verification."""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
from pathlib import Path
from blake3 import blake3


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def read_rows(path):
    require(path.stat().st_size <= 64 * 1024**2, "trace total budget")
    lines = path.read_bytes().splitlines(keepends=True)
    require(len(lines) == 30 and all(len(line) <= 65536 for line in lines), "frame count/record budget")
    return [json.loads(line) for line in lines]


def legacy_rows(rows):
    return [{key: value for key, value in row.items() if key not in
        {"processingQpc100ns", "receiverDecisions", "receiverDataDispositionAuthority"}} for row in rows]


def analyze(root):
    repo = Path(__file__).resolve().parents[2]
    original = repo / "artifacts/remote-step3b-20260908-run01"
    expected = (original / "source/step3b-source.bin").read_bytes()
    require(len(expected) == 65536 and hashlib.sha256(expected).hexdigest() ==
        "40f1859cccb1f6949e8ff0a0c157c97571759a020d7bc3c801bcdc457394276c", "original source identity")
    reservation = read_json(root / "NORMAL_OBSERVATION_RESERVATION.json")
    require(reservation["observations"] == 60 and reservation["runs"] == 2, "normal observation reservation")
    summaries, rows, files = {}, {}, {}
    for mode in ["off", "on"]:
        run = root / f"runs/{mode}"
        process = read_json(root / f"logs/raw-{mode}.process.json")
        require(process["returnCode"] == 0 and process["failures"] == [], "child execution failure")
        require(process["jobAssignedBeforeResume"] and process["processCommitLimitBytes"] == 2 * 1024**3 and
            process["timeoutSeconds"] <= 600, "child resource supervision")
        summary = read_json(run / "summary.json")
        require(summary["receiverDecisionDiagnostics"] == (mode == "on"), "diagnostic mode")
        require(summary["frames"] == 30 and summary["reachedEof"] and summary["publishedAndReopened"] and not summary["error"], "whole run incomplete")
        require(summary["fieldStatus"] == "NOT_RUN" and summary["liveChannelGoodput"] is None, "offline evidence promoted to field")
        report = summary["receiverReport"]
        for key in ["wholeDigestVerified", "renameSucceeded", "finalReopenVerified", "published"]:
            require(report["publish"][key] is True, f"publish gate {key}")
        recovery = report["recovery"]
        require(recovery["verifiedSegments"] == 1 and recovery["verifiedRawBytes"] == 65536 and
            recovery["verifiedEncodedSegmentBytes"] == 65536 and not recovery["resumeLoaded"], "recovery/new state mismatch")
        final = Path(report["publish"]["finalPath"]).resolve()
        require(final.is_relative_to((run / "output").resolve()), "unexpected publication target")
        data = final.read_bytes()
        require(data == expected, "published whole file differs from original")
        files[mode] = {"path": str(final), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "blake3": blake3(data).hexdigest()}
        rows[mode] = read_rows(run / "frames.jsonl")
        require(all(row["bootstrapAccepted"] and row["acceptedBlocks"] == 15 for row in rows[mode]), "raw decode regression")
        require(all(row["receiverDataDispositionAuthority"] == "LegacyDefaultDoNotUseForUnified" for row in rows[mode]), "legacy field authority missing")
        summaries[mode] = report
    require(legacy_rows(rows["off"]) == legacy_rows(rows["on"]), "on/off changed non-clock decisions/payload digests")
    require(legacy_rows(rows["off"]) == legacy_rows(read_rows(original / "raw-full/frames.jsonl")), "original sealed raw behavior changed")
    require(all(row["receiverDecisions"] is None for row in rows["off"]), "disabled trace still emitted decisions")
    reasons, slot_reasons = collections.Counter(), collections.Counter()
    unique, data_calls, control_calls = 0, 0, 0
    for row in rows["on"]:
        decision = row["receiverDecisions"]
        require(decision["schema"] == "PixelBridge.ReceiverDecisions.1", "decision schema")
        reasons[decision["frameReason"]] += 1
        slots = decision["slots"]
        indexes = [slot["slot"] for slot in slots]
        require(len(indexes) <= 15 and len(set(indexes)) == len(indexes) and all(0 <= index < 15 for index in indexes), "slot capacity/identity")
        for slot in slots:
            slot_reasons[slot["reason"]] += 1
            if slot["dataDisposition"] is not None:
                require(slot["kind"] == 0 and slot["receiverCalled"] and slot["receiverReturned"] and slot["outerSymbolAdmission"] is not None,
                    "fabricated non-null data disposition")
            if not slot["receiverCalled"]:
                require(not slot["receiverReturned"] and slot["dataDisposition"] is None and slot["outerSymbolAdmission"] is None, "uncalled Receiver returned a value")
            unique += slot["outerSymbolAdmission"] == 1
            data_calls += slot["kind"] == 0 and slot["receiverCalled"]
            control_calls += slot["kind"] == 1 and slot["receiverCalled"]
    counter_keys = ["outerUniqueSymbols", "outerIdenticalDuplicateSymbols", "outerConflictRejections", "outerResourceRejections",
        "outerDeferredResourceBusyCount", "outerOrphanAdmittedBlockCount", "outerOrphanDroppedByQuotaCount", "outerOrphanConflictRejectionCount",
        "bootstrapAcceptedFrames", "bootstrapRejectedFrames"]
    counter_values = {mode: {key: summaries[mode]["stageCounters"][key] for key in counter_keys} for mode in ["off", "on"]}
    require(counter_values["off"] == counter_values["on"], "on/off Receiver counters differ")
    require(unique == counter_values["on"]["outerUniqueSymbols"] and unique > 0, "per-slot unique accounting differs from authority")
    for key in ["outerConflictRejections", "outerResourceRejections", "outerDeferredResourceBusyCount", "outerOrphanDroppedByQuotaCount", "outerOrphanConflictRejectionCount"]:
        require(counter_values["on"][key] == 0, "unexpected raw recovery pressure/conflict")
    require(reasons["AlreadyPublished"] > 0 and rows["on"][0]["receiverDecisions"]["before"]["sessionReady"] is False and
        rows["on"][0]["receiverDecisions"]["after"]["sessionReady"] is True, "binding/publication state diagnostic missing")
    return {"schema": "PixelBridge.ReceiverDecisionParity.1", "status": "PASS", "normalPixelObservations": 60,
        "diagnosticsOnOffLegacyTraceEqual": True, "sealedOriginalLegacyTraceEqual": True, "wholeFiles": files,
        "receiverCounters": counter_values["on"], "frameReasons": dict(reasons), "slotReasons": dict(slot_reasons),
        "actualDataCalls": data_calls, "actualControlCalls": control_calls, "perSlotUniqueSymbols": unique,
        "fieldStatus": "NOT_RUN", "liveChannelGoodput": None, "speedupClaim": None}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.root.resolve())
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
            stream.write("\n")
    print(json.dumps(result, indent=2))
