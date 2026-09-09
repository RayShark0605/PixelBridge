"""Read-only R1 attribution from saved hashes and journal; no media/Receiver replay."""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
import zipfile
from blake3 import blake3


def analyze(repo):
    evidence = repo / "artifacts/remote-step2-20260908-run01"
    identity = json.loads((evidence / "identity/recording-01-source-identity.json").read_text(encoding="utf-8-sig"))
    identities = {item["path"]: item["sha256"] for item in identity["files"]}
    source_checks = {}
    with zipfile.ZipFile(evidence / "identity/recording-01-source-overlay.zip") as archive:
        for name in ["apps/common/local_desktop_runtime.cpp", "apps/common/recorded_pixel_replay.inc",
                "apps/common/decoder_resume_store.cpp", "apps/common/sender_carousel_scheduler.cpp",
                "apps/common/sender_carousel_scheduler.h", "libs/PBProtocol/src/bootstrap_control_codec.cpp",
                "libs/PBProtocol/include/pbprotocol/bootstrap_control_codec.h"]:
            data = archive.read(name) if name in archive.namelist() else (repo / name).read_bytes()
            source_checks[name] = hashlib.sha256(data).hexdigest()
            if source_checks[name] != identities[name]:
                raise ValueError(f"R1 explanatory source mismatch: {name}")
    trace_path = evidence / "runs/recording-01/frames.jsonl"
    journal_path = evidence / "runs/recording-01/output/PixelBridge-5d04e895a2b4254a.resume"
    if trace_path.stat().st_size > 8 * 1024**2 or journal_path.stat().st_size > 8 * 1024**2:
        raise ValueError("Metadata input byte budget exceeded")
    trace_bytes, journal = trace_path.read_bytes(), journal_path.read_bytes()
    if hashlib.sha256(trace_bytes).hexdigest() != "e32407376ba4bbcd96e09dee93bf805eb7330d6b347763b9db55aa7df9012488":
        raise ValueError("R1 trace identity changed")
    if hashlib.sha256(journal).hexdigest() != "c3ca9c31eda80c3e06834830495becb18bc8b3183b5bb324c3842931a73b682a":
        raise ValueError("Inspected R1 journal identity changed")
    table = []
    for number in range(256):
        value = number
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)

    def crc(data):
        value = 0xFFFFFFFF
        for number in data:
            value = (value >> 8) ^ table[(value ^ number) & 255]
        return value ^ 0xFFFFFFFF

    def check_crc(data):
        if len(data) < 4 or crc(data[:-4]) != struct.unpack_from("<I", data, len(data) - 4)[0]:
            raise ValueError("Journal/envelope CRC mismatch")

    if crc(b"123456789") != 0xE3069283:
        raise ValueError("CRC32C check vector failed")
    magic, version, reserved, tag, session_size, reserved2 = struct.unpack_from("<4sHHQII", journal)
    if (magic, version, reserved, reserved2) != (b"PBJH", 1, 0, 0) or session_size > 65536:
        raise ValueError("Journal header invalid")
    offset = 28 + session_size
    if offset > len(journal):
        raise ValueError("Truncated header")
    check_crc(journal[:offset])
    controls = [("session", journal[24:24 + session_size])]
    generation = 0
    record_types = collections.Counter()
    while offset < len(journal):
        if len(journal) - offset < 36:
            raise ValueError("Truncated journal tail")
        magic, version, kind, total, size, current_generation, reserved = struct.unpack_from("<4sHHIIQQ", journal, offset)
        if magic != b"PBJR" or version != 1 or kind not in range(1, 7) or total != size + 36 or current_generation <= generation or reserved or total > len(journal) - offset:
            raise ValueError("Journal record shape invalid")
        record = journal[offset:offset + total]
        check_crc(record)
        if kind in (1, 2):
            controls.append(("segment" if kind == 1 else "manifest", record[32:-4]))
        record_types[kind] += 1
        generation, offset = current_generation, offset + total
    digest_map, control_evidence = {}, []
    for label, data in controls:
        check_crc(data)
        magic, version, kind, sequence, session_tag, total = struct.unpack_from("<4sBBQQI", data)
        if magic != b"PBCR" or version != 1 or session_tag != tag or total != len(data) or kind != {"session": 1, "segment": 2, "manifest": 3}[label]:
            raise ValueError("Canonical saved control envelope invalid")
        digest = blake3(data).hexdigest()
        digest_map[digest] = label
        control_evidence.append({"type": label, "bytes": len(data), "sequence": sequence, "blake3": digest})
    rows = [json.loads(line) for line in trace_bytes.splitlines()]
    if len(rows) != 3622:
        raise ValueError("R1 row count changed")
    first, session_rows = {}, []
    for row in rows:
        if row["bootstrapAccepted"]:
            first.setdefault((row["sessionTag"], row["frameSequence"]), row)
        if any(kind == 1 and digest_map[digest] == "session" for _, kind, _, digest in row["acceptedPayloadDigests"]):
            session_rows.append(row)
    transition = session_rows[0]
    phases = {}
    for label, subset in [("beforeSession", [r for r in first.values() if r["observation"] < transition["observation"]]),
                          ("afterSession", [r for r in first.values() if r["observation"] >= transition["observation"]])]:
        control_counts, transport_digests = collections.Counter(), []
        for row in subset:
            for _, kind, _, digest in row["acceptedPayloadDigests"]:
                if kind == 1:
                    control_counts[digest_map[digest]] += 1
                else:
                    transport_digests.append(digest)
        phases[label] = {"uniqueAcceptedFrames": len(subset), "controlSlots": dict(control_counts),
            "transportSlots": len(transport_digests), "distinctTransportByteDigests": len(set(transport_digests))}
    return {"schema": "PixelBridge.R1ControlAttribution.1", "classification": "HistoricalMetadataAnalysis",
        "sourceFingerprint": identity["sourceFingerprint"], "verifiedR1Source": source_checks,
        "traceSha256": hashlib.sha256(trace_bytes).hexdigest(), "journalSha256": hashlib.sha256(journal).hexdigest(),
        "journalAllCrcValid": True, "journalRecordTypes": {str(key): value for key, value in record_types.items()}, "savedControls": control_evidence,
        "firstAcceptedSession": {key: transition[key] for key in ["observation", "pts", "timeBaseNumerator", "timeBaseDenominator", "frameSequence"]},
        "phases": phases, "legacyDispositionValues": {str(key): value for key, value in collections.Counter(row["receiverDataDisposition"] for row in rows).items()},
        "noNewMediaOrPixelObservations": True, "savedSeconds": None, "liveChannelGoodput": None,
        "limits": "Cannot locate upstream loss from Receiver-only trace; rejected frames have no decoded payload; distinct transport digests are not verified file bytes"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(Path(__file__).resolve().parents[2])
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
            stream.write("\n")
    print(json.dumps(result, indent=2))
