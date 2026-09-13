#!/usr/bin/env python3
"""Independent PB-Experimental-GrayFast-1 (layout 13) CPU-raster oracle Golden.

The gray-fast identity keeps the layout-12 seven-plane gray raster, mapping,
codebook, interleave and slot plan byte-for-byte, and swaps the inner
Robust QC-LDPC (2/3) for the frozen DVB-S2 Short Fast profile (37/45):
1665 information bytes and 1629-byte Transport payloads per codeword. Like
the gray-state generator this script imports only earlier independent Python
oracles (never C++ headers, executables or build artifacts); the Fast
parity-check shift table below is transcribed verbatim from
ETSI EN 302 307-1 Table 5b (K=13320, N-K=2880, Q=8) as carried in
libs/PBInnerFec/src/dvbs2_short_matrix.h. Generation is create-only; --check
is byte-for-byte and validates exact inventory.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys

from blake3 import blake3

sys.dont_write_bytecode = True
import generate_unified_sc6_cpu_golden as sc6cpu
import generate_unified_sc6_mapping_golden as mapping
import generate_unified_graystates_golden as graystates

DEFAULT_ROOT = Path(__file__).resolve().parents[1] / "golden" / "unified-grayfast-cpu-oracle"
PROFILE_ID = 0x5042475246535431
LAYOUT_VERSION = 13
PROFILE_NAME = "PB-Experimental-GrayFast-1"
INFO_BYTES = 1665
TRANSPORT_PAYLOAD_LIMIT = 1629
LOW, HIGH, NEUTRAL = 8, 160, 128
LUMA_LEVELS = (8, 64, 160, 232)
VARIANTS = graystates.VARIANTS
LOW_FOREGROUND = graystates.LOW_FOREGROUND
HIGH_FOREGROUND = graystates.HIGH_FOREGROUND
GRAY_PLANES = graystates.GRAY_PLANES

# DVB-S2 Short FECFRAME Fast parity-check shifts (Table 5b, rate 37/45):
# thirty-seven lines (360 information bits each), degree 13 on line 0 and
# degree 3 elsewhere; the accumulator step is Q = 2880/360 = 8.
FAST_SHIFTS = (
    (3, 2409, 499, 1481, 908, 559, 716, 1270, 333, 2508, 2264, 1702, 2805),
    (4, 2447, 1926), (5, 414, 1224), (6, 2114, 842), (7, 212, 573),
    (0, 2383, 2112), (1, 2286, 2348), (2, 545, 819), (3, 1264, 143),
    (4, 1701, 2258), (5, 964, 166), (6, 114, 2413), (7, 2243, 81),
    (0, 1245, 1581), (1, 775, 169), (2, 1696, 1104), (3, 1914, 2831),
    (4, 532, 1450), (5, 91, 974), (6, 497, 2228), (7, 2326, 1579),
    (0, 2482, 256), (1, 1117, 1261), (2, 1257, 1658), (3, 1478, 1225),
    (4, 2511, 980), (5, 2320, 2675), (6, 435, 1278), (7, 228, 503),
    (0, 1885, 2369), (1, 57, 483), (2, 838, 1050), (3, 1231, 1990),
    (4, 1738, 68), (5, 2392, 951), (6, 163, 645), (7, 2644, 1704))


def Record(sequence: int) -> bytes:
    prefix = struct.pack("<4sBBBBQQQII", b"PBRG", 1, 1, 0, LAYOUT_VERSION, PROFILE_ID,
                         sc6cpu.SESSION_TAG, sequence, 0x21222324, 0)
    assert len(prefix) == 40
    import generate_local_desktop_golden as scaffold
    return prefix + struct.pack("<I", scaffold.Crc32C(prefix))


def FastEncode(information: bytes) -> bytes:
    assert len(information) == INFO_BYTES
    parity = 0
    for bit in range(13320):
        if information[bit // 8] & (1 << (bit % 8)):
            for shift in FAST_SHIFTS[bit // 360]:
                parity ^= 1 << ((shift + (bit % 360) * 8) % 2880)
    carry = 0
    packed = bytearray(360)
    for bit in range(2880):
        carry ^= (parity >> bit) & 1
        packed[bit // 8] |= carry << (bit % 8)
    return information + bytes(packed)


def LargeTransport(slot: int) -> bytes:
    # Near-ceiling payloads freeze the 1629-byte Transport payload contract
    # that distinguishes this identity from layout 12.
    import generate_local_desktop_golden as scaffold
    payload_size = TRANSPORT_PAYLOAD_LIMIT - 4 - slot % 7
    payload = bytes((slot * 53 + index * 29 + 11) & 0xFF for index in range(payload_size))
    header = struct.pack("<BBHQQIHH", 1, 0, 0, sc6cpu.SESSION_TAG, 700 + slot, 800 + slot,
                         len(payload), 0)
    header += struct.pack("<I", scaffold.Crc32C(header))
    assert len(header) == 32
    block = header + payload + struct.pack("<I", scaffold.Crc32C(payload))
    return block


def MixedCodewords18() -> tuple[bytes, tuple[bytes, ...]]:
    blocks = (sc6cpu.Control(),) + tuple(LargeTransport(slot) for slot in range(1, 18))
    coded = bytearray()
    for block in blocks:
        information = bytearray(INFO_BYTES)
        assert len(block) <= INFO_BYTES
        information[:len(block)] = block
        coded += FastEncode(bytes(information))
    assert len(coded) == 18 * 2025
    return bytes(coded), blocks


def Raster(sequence: int, record: bytes, codewords: bytes) -> bytes:
    # The gray-fast raster is the layout-12 seven-plane raster verbatim; only
    # the codeword bits differ (Fast parity).
    return graystates.Raster(sequence, record, codewords)


def BuildFiles() -> dict[str, bytes]:
    mapping.ValidateFrozenConstants()
    previous_record = Record(40)
    current_record = Record(41)
    codewords, blocks = MixedCodewords18()
    previous = Raster(40, previous_record, codewords)
    current = Raster(41, current_record, codewords)
    digest_bytes, digest_map = graystates.RasterDigests(previous, current)
    accepted = bytearray()
    for slot, block in enumerate(blocks):
        accepted += struct.pack("<B", slot)
        # Accepted stream layout mirrors the gray-state Golden: slot index
        # byte + block length u16 + padded block.
        accepted += struct.pack("<H", len(block)) + block
    contract = {
        "artifact": f"{PROFILE_NAME} CPU oracle Golden",
        "canvas": {"width": 1920, "height": 1080, "pixelFormat": "BGRA8_UNORM_SDR"},
        "codewords": {"count": 18, "bytesEach": 2025, "informationBytesEach": INFO_BYTES,
                      "innerFecProfile": "DVB-S2-Short-N16200-K13320"},
        "data": {"lowLuma": LOW, "highLuma": HIGH, "neutralLuma": NEUTRAL,
                 "tiles": mapping.DATA_TILE_COUNT, "tilePixels": 6, "glyphPixels": 5,
                 "separatorPixels": 1, "foregroundStates": "gray", "foregroundLevels": [64, 232]},
        "transport": {"payloadLimitBytes": TRANSPORT_PAYLOAD_LIMIT,
                      "slotPayloadPattern": "1629-4-slot%7"},
        "profile": {"name": PROFILE_NAME, "profileIdHex": f"{PROFILE_ID:016x}", "layoutVersion": LAYOUT_VERSION},
        "slotPlan": {"slot0": "Control/SessionDescriptor", "slots1To17": "Transport"},
        "variants": list(VARIANTS),
        "independentRebuild": "tests/PBModulation/generate_unified_grayfast_golden.py --check",
        "digestsSha256Of": "raster-digests.bin",
        "acceptedStream": "18 slot-framed near-ceiling Transport blocks",
    }
    return {
        "accepted-stream.bin": sc6cpu.AcceptedStream(blocks),
        "bootstrap-sequence40.bin": previous_record,
        "bootstrap-sequence41.bin": current_record,
        "contract.json": (json.dumps(contract, indent=2, sort_keys=True) + chr(10)).encode(),
        "mixed-codewords.bin": codewords,
        "raster-digests.bin": digest_bytes,
    }


def WriteFiles(root: Path, files: dict[str, bytes]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    for name, payload in files.items():
        (root / name).write_bytes(payload)
    print(f"GRAYFAST_GOLDEN_WRITTEN {root} ({len(files)} files)")


def CheckFiles(root: Path, files: dict[str, bytes]) -> None:
    actual = sorted(p.name for p in root.iterdir() if p.is_file())
    expected = sorted(files)
    assert actual == expected, (actual, expected)
    for name, payload in files.items():
        assert (root / name).read_bytes() == payload, name
    print(f"GRAYFAST_GOLDEN_CHECK_OK {root}")


def Main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    files = BuildFiles()
    if arguments.check:
        CheckFiles(arguments.golden_root, files)
        return
    WriteFiles(arguments.golden_root, files)


if __name__ == "__main__":
    Main()
