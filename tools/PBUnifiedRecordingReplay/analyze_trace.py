"""Read-only aggregation of bounded recording evidence, not field goodput."""
import argparse
from collections import Counter
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--new-output", type=Path, required=True)
    args = parser.parse_args()
    trace = args.run / "frames.jsonl"
    assert trace.stat().st_size <= 64 * 1024 * 1024
    report = json.loads((args.run / "summary.json").read_text(encoding="utf-8"))
    bootstrap = Counter()
    lanes = [Counter() for _ in range(3)]
    unique = set()
    accepted_slots = set()
    freshness = Counter()
    frame_erasures = Counter()
    observations = 0
    with trace.open(encoding="utf-8") as stream:
        for line in stream:
            assert len(line) <= 65536 and observations < 7200
            row = json.loads(line)
            observations += 1
            bootstrap[str(row["bootstrapAccepted"])] += 1
            if row["bootstrapAccepted"]:
                identity = (row["sessionTag"], row["frameSequence"])
                unique.add(identity)
                frame_erasures[str(row["frameErasure"])] += 1
                for lane, rejection, iterations, fec, crc, accepted in row["slots"]:
                    stats = lanes[lane]
                    stats["observedSlots"] += 1
                    stats["fecValid"] += fec
                    stats["crcValid"] += crc
                    stats["accepted"] += accepted
                    stats["iterations"] += iterations
                    stats[f"rejection-{rejection}"] += 1
                for slot, kind, size, digest in row["acceptedPayloadDigests"]:
                    accepted_slots.add((*identity, slot, kind, size, digest))
                for current in row["freshnessCurrent"]:
                    freshness["current" if current else "notCurrent"] += 1
    stages = report["diagnostics"]["stages"] if report.get("diagnostics") else {}
    result = {
        "schema": "PixelBridge.RecordingLossAnalysis.1",
        "classification": "OfflineRecordingDiagnostic",
        "observations": observations, "bootstrap": bootstrap,
        "distinctBootstrapIdentities": len(unique),
        "repeatedBootstrapObservations": bootstrap["True"] - len(unique),
        "distinctAcceptedSlotIdentityAndDigest": len(accepted_slots),
        "frameErasureCodesOnAcceptedBootstrap": frame_erasures,
        "laneOrder": ["BaseLuma", "FineLuma", "Chroma"],
        "laneObservationCounters": lanes,
        "freshnessOnAcceptedBootstrap": freshness,
        "receiverCounters": report.get("receiverReport", {}).get("stageCounters"),
        "localInclusiveStageMilliseconds": {key: value["totalNanoseconds"] / 1e6
            for key, value in stages.items() if value["totalNanoseconds"] is not None},
        "recovery": report.get("receiverReport", {}).get("recovery"),
        "publishedAndReopened": report["publishedAndReopened"],
        "reachedEof": report["reachedEof"], "error": report["error"],
        "limitations": [
            "No original capture cadence, cursor, backend or sender timing proof",
            "Repeated observations included in lane totals; distinct slot count is a separate union",
            "Inclusive/nested CPU timers must not be added together; WARP is not physical GPU speed",
            "Decoder EOF is not proof the recording covered the complete transfer",
            "No sender equation truth; BER and false-acceptance rate unavailable",
            "No live channel performance, remote certification, or Step3 promotion",
        ],
    }
    with args.new_output.open("x", encoding="utf-8") as output:
        json.dump(result, output, ensure_ascii=False, indent=2)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
