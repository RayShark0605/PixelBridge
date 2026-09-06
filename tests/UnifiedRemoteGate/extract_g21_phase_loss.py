"""Extract only accepted-ID loss shape from a sealed G21 journal (no payload export)."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def crc32c(data):
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)
    result = 0xFFFFFFFF
    for value in data:
        result = table[(result ^ value) & 255] ^ (result >> 8)
    return result ^ 0xFFFFFFFF


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("journal", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.journal.stat().st_size != 12373056:
        raise ValueError("Journal size differs from the sealed bounded artifact")
    with args.journal.open("rb") as journal:
        data = journal.read(12373057)
    if len(data) != 12373056:
        raise ValueError("Journal size changed during bounded read")
    digest = hashlib.sha256(data).hexdigest()
    if digest != "77d5e91dd51d50d9d6c11d7872b2153dbda86b22f197d52e3216fdab89d380d2":
        raise ValueError("Not the sealed 786f466 Segment-27 failure journal")
    if data[:4] != b"PBJH":
        raise ValueError("Invalid header")
    offset = 28 + struct.unpack_from("<I", data, 16)[0]
    if crc32c(data[:offset - 4]) != struct.unpack_from("<I", data, offset - 4)[0]:
        raise ValueError("Header CRC mismatch")
    accepted = set()
    previous_generation = 0
    records = 0
    while offset < len(data):
        magic, version, kind, size, payload_size, generation, reserved = struct.unpack_from("<4sHHIIQQ", data, offset)
        if (magic != b"PBJR" or version != 1 or not 1 <= kind <= 6 or size < 36
                or size != payload_size + 36 or offset + size > len(data)
                or generation <= previous_generation or reserved):
            raise ValueError("Invalid bounded journal record")
        record = data[offset:offset + size]
        if crc32c(record[:-4]) != struct.unpack_from("<I", record, size - 4)[0]:
            raise ValueError("Record CRC mismatch")
        if kind == 3:
            ordinal, block_id, declared, reserved_block, padded = struct.unpack_from("<QIHHI", record, 32)
            if reserved_block or not 0 < declared <= padded or padded != 1314 or payload_size != 20 + padded:
                raise ValueError("Invalid accepted-block dimensions")
            if ordinal == 27:
                if block_id >= 7216:
                    raise ValueError("Unexpected equation outside frozen Pass-0 budget")
                accepted.add(block_id)
        previous_generation = generation
        records += 1
        offset += size
    if len(accepted) != 6375:
        raise ValueError("Sealed accepted count changed")
    # 14 is the ordinary mixed frame's Transport capacity. This is a conservative
    # band projection, NOT a reconstructed capture timestamp or exact frame trace.
    # A band is erased if any ID in it was not durably accepted; other phases are
    # unspecified by the journal and are separately declared synthetic controls.
    erased_bands = [band for band in range((7216 + 13) // 14)
                    if any(block not in accepted for block in range(band * 14, min((band + 1) * 14, 7216)))]
    result = {
        "schema": "PixelBridge.G21.PhaseLossProjection.1", "sourcePath": str(args.journal.resolve()),
        "sourceSha256": digest, "sourceBytes": len(data), "validatedRecords": records,
        "segmentOrdinal": 27, "systematicBlocks": 6385, "scheduledBlocks": 7216,
        "acceptedUniqueBlocks": len(accepted), "missingBlocks": 7216 - len(accepted),
        "bandWidth": 14, "bandCount": (7216 + 13) // 14, "erasedBands": erased_bands,
        "authority": "CRC-validated durable accepted IDs projected to 14-equation bands; not capture timestamps",
        "otherPhases": "No observations inferable from completed-Segment journal; any other-phase loss is synthetic",
    }
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
