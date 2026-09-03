#!/usr/bin/env python3
"""Independent PB-Unified-LC4-V1 CPU-raster oracle.

The generator imports only earlier independent Python oracles. It does not read
C++ headers, invoke a PixelBridge executable, or use generated build artifacts.
Generation is create-only; --check is byte-for-byte and validates exact inventory.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys

from blake3 import blake3

sys.dont_write_bytecode = True
import generate_desktop_levels_golden as desktop
import generate_local_desktop_golden as scaffold
import generate_unified_lc4_mapping_golden as mapping


DEFAULT_ROOT = Path(__file__).resolve().parents[1] / "golden" / "unified-lc4-cpu-oracle"
PROFILE_ID = 0x5042554E494C4331
SESSION_TAG = 0x1122334455667788
CODEWORD_BYTES = 2025
INFO_BYTES = 1350
CODEWORD_COUNT = 31
LOW, HIGH, NEUTRAL = 80, 176, 128
LUMA_LEVELS = (32, 80, 176, 224)
CALIBRATION = ((736, 16), (1696, 16), (96, 1000), (1056, 1000))
PHASE = ((896, 16), (896, 1000))
DATA_REGIONS = (
    (96, 96, 1728, 64), (224, 160, 672, 128), (1024, 160, 672, 128),
    (96, 288, 1728, 188), (224, 476, 672, 128), (1024, 476, 672, 128),
    (96, 604, 1728, 188), (224, 792, 672, 128), (1024, 792, 672, 128),
    (96, 920, 1728, 64))
VARIANTS = ("clean", "chroma-neutralized", "base-neutralized", "fine-neutralized",
            "localized-stale", "wrong-sequence")


def Record(sequence: int) -> bytes:
    prefix = struct.pack("<4sBBBBQQQII", b"PBRG", 1, 1, 0, 8, PROFILE_ID,
                         SESSION_TAG, sequence, 0x21222324, 0)
    assert len(prefix) == 40
    return prefix + struct.pack("<I", scaffold.Crc32C(prefix))


def RobustEncode(information: bytes) -> bytes:
    assert len(information) == INFO_BYTES
    parity = 0
    for bit in range(10800):
        if information[bit // 8] & (1 << (bit % 8)):
            for shift in desktop.ROBUST_SHIFTS[bit // 360]:
                parity ^= 1 << ((shift + (bit % 360) * 15) % 5400)
    carry = 0
    packed = bytearray(675)
    for bit in range(5400):
        carry ^= (parity >> bit) & 1
        packed[bit // 8] |= carry << (bit % 8)
    return information + bytes(packed)


def Transport(slot: int, sequence: int) -> bytes:
    payload_size = 48 + slot % 13
    payload = bytes((slot * 37 + index * 11 + 5) & 0xFF for index in range(payload_size))
    header = struct.pack("<BBHQQIHH", 1, 0, 0, SESSION_TAG, 900 + slot, 1000 + slot,
                         len(payload), 0)
    header += struct.pack("<I", scaffold.Crc32C(header))
    assert len(header) == 32
    block = header + payload + struct.pack("<I", scaffold.Crc32C(payload))
    assert sequence >= 0  # Sequence deliberately does not enter Transport identity.
    return block


def Control() -> bytes:
    payload = bytes((1, 3, 5, 7, 9, 11, 13, 15, 17))
    record_bytes = 26 + len(payload) + 4
    prefix = struct.pack("<4sBBQQI", b"PBCR", 1, 1, 77, SESSION_TAG, record_bytes)
    assert len(prefix) == 26
    content = prefix + payload
    return content + struct.pack("<I", scaffold.Crc32C(content))


def MixedCodewords(sequence: int) -> tuple[bytes, tuple[bytes, ...]]:
    blocks = (Control(),) + tuple(Transport(slot, sequence) for slot in range(1, CODEWORD_COUNT))
    codewords = bytearray()
    for block in blocks:
        assert len(block) <= INFO_BYTES
        codewords += RobustEncode(block + bytes(INFO_BYTES - len(block)))
    assert len(codewords) == CODEWORD_COUNT * CODEWORD_BYTES
    return bytes(codewords), blocks


def PhaseLabel(fine: bool, tile: int, sequence: int) -> int:
    phase = sequence % 16
    if not fine:
        return (tile + phase) & 7
    return (((tile // 2) + phase) & 7) | (((tile + phase) & 1) << 3)


def Fill(image: bytearray, x: int, y: int, width: int, height: int,
         blue: int, green: int | None = None, red: int | None = None) -> None:
    green = blue if green is None else green
    red = blue if red is None else red
    row = bytes((blue, green, red, 255)) * width
    for target_y in range(y, y + height):
        offset = (target_y * scaffold.WIDTH + x) * 4
        image[offset:offset + len(row)] = row


def Raster(sequence: int, record: bytes, codewords: bytes) -> bytes:
    image = bytearray(scaffold.Raster(scaffold.EncodeRs(record), scaffold.TimingBits(record),
                                      scaffold.MarkerBits()))
    for x, y in CALIBRATION:
        for label, level in enumerate(LUMA_LEVELS):
            Fill(image, x + label * 32, y, 32, 24, level)
        Fill(image, x, y + 24, 128, 8, NEUTRAL)
        for label, (blue, green, red, state_label) in enumerate(mapping.CHROMA_STATES_BY_LABEL):
            assert label == state_label
            Fill(image, x + label * 32, y + 32, 32, 32,
                 NEUTRAL + blue, NEUTRAL + green, NEUTRAL + red)
    for fine, (x, y) in enumerate(PHASE):
        for tile in range((128 // 4) * (64 // 4)):
            tile_x = x + (tile % 32) * 4
            tile_y = y + (tile // 32) * 4
            mask = mapping.MASKS_BY_LABEL[PhaseLabel(bool(fine), tile, sequence)]
            for chip in range(16):
                Fill(image, tile_x + chip % 4, tile_y + chip // 4, 1, 1,
                     HIGH if mask & (1 << chip) else LOW)

    luma_labels = bytearray(mapping.DATA_TILE_COUNT)
    chroma_labels = bytearray(mapping.DATA_TILE_COUNT)
    lane_starts = (0, 17 * 16200, 21 * 16200)
    for lane, lane_contract in enumerate(mapping.LANES):
        for logical_bit in range(lane_contract[1]):
            carrier, tile, plane = mapping.MapLogicalBit(lane, logical_bit, sequence)
            global_bit = lane_starts[lane] + logical_bit
            value = (codewords[global_bit // 8] >> (global_bit % 8)) & 1
            if carrier == 0:
                luma_labels[tile] |= value << plane
            else:
                chroma_labels[tile] |= value << plane
    coordinates = desktop.Coordinates(4)
    assert len(coordinates) == mapping.DATA_TILE_COUNT
    for tile, (x, y) in enumerate(coordinates):
        mask = mapping.MASKS_BY_LABEL[luma_labels[tile]]
        blue, green, red, state_label = mapping.CHROMA_STATES_BY_LABEL[chroma_labels[tile]]
        assert state_label == chroma_labels[tile]
        for chip in range(16):
            base = HIGH if mask & (1 << chip) else LOW
            Fill(image, x + chip % 4, y + chip // 4, 1, 1,
                 base + blue, base + green, base + red)
    return bytes(image)


def CopyRegion(source: bytes, destination: bytearray,
               region: tuple[int, int, int, int]) -> None:
    x, y, width, height = region
    row_bytes = width * 4
    for row in range(height):
        offset = ((y + row) * scaffold.WIDTH + x) * 4
        destination[offset:offset + row_bytes] = source[offset:offset + row_bytes]


def FreshnessRegion(x: int, y: int) -> int:
    center_x, center_y = x + 2, y + 2
    column = 0 if center_x < 560 else 1 if center_x < 1360 else 2
    row = 0 if center_y < 382 else 1 if center_y < 698 else 2
    return row * 3 + column


def CopyFreshnessData(source: bytes, destination: bytearray, freshness_region: int) -> None:
    for x, y in desktop.Coordinates(4):
        if FreshnessRegion(x, y) == freshness_region:
            CopyRegion(source, destination, (x, y, 4, 4))


def NeutralizeChroma(image: bytearray) -> None:
    for offset in range(0, len(image), 4):
        blue, green, red = image[offset:offset + 3]
        level = int(0.0722 * blue + 0.7152 * green + 0.2126 * red + 0.5)
        image[offset:offset + 3] = bytes((level, level, level))


def RasterDigests(previous: bytes, current: bytes) -> tuple[bytes, dict[str, str]]:
    variants: list[bytes] = [current]
    chroma_neutral = bytearray(current)
    NeutralizeChroma(chroma_neutral)
    variants.append(bytes(chroma_neutral))
    base_neutral = bytearray(current)
    Fill(base_neutral, 896, 16, 128, 64, NEUTRAL)
    variants.append(bytes(base_neutral))
    fine_neutral = bytearray(current)
    Fill(fine_neutral, 896, 1000, 128, 64, NEUTRAL)
    variants.append(bytes(fine_neutral))
    localized_stale = bytearray(current)
    CopyRegion(previous, localized_stale, (896, 476, 128, 128))
    CopyFreshnessData(previous, localized_stale, 4)
    variants.append(bytes(localized_stale))
    wrong_sequence = bytearray(current)
    for region in DATA_REGIONS:
        CopyRegion(previous, wrong_sequence, region)
    variants.append(bytes(wrong_sequence))
    assert len(variants) == len(VARIANTS)
    digests = tuple(blake3(value).digest() for value in variants)
    return b"".join(digests), {name: digest.hex() for name, digest in zip(VARIANTS, digests, strict=True)}


def AcceptedStream(blocks: tuple[bytes, ...]) -> bytes:
    result = bytearray()
    for slot, block in enumerate(blocks):
        result += struct.pack("<BBH", slot, int(slot != 0), len(block))
        result += block
    return bytes(result)


def BuildFiles() -> dict[str, bytes]:
    assert blake3(b"").hexdigest() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
    mapping.ValidateFrozenConstants()
    previous_record = Record(40)
    current_record = Record(41)
    codewords, blocks = MixedCodewords(41)
    previous = Raster(40, previous_record, codewords)
    current = Raster(41, current_record, codewords)
    digest_bytes, digest_map = RasterDigests(previous, current)
    contract = {
        "artifact": "PB-Unified-LC4-V1 CPU oracle Golden",
        "canvas": {"width": 1920, "height": 1080, "pixelFormat": "BGRA8_UNORM_SDR"},
        "codewords": {"count": 31, "bytesEach": 2025, "informationBytesEach": 1350,
                      "innerFecProfile": "DVB-S2-Short-N16200-K10800"},
        "data": {"lowLuma": LOW, "highLuma": HIGH, "neutralLuma": NEUTRAL,
                 "tiles": mapping.DATA_TILE_COUNT, "tilePixels": 4},
        "pilots": {"lumaRows": 24, "neutralRows": 8, "chromaRows": 32,
                   "lumaLevels": LUMA_LEVELS},
        "freshnessPartition": {"columnBoundaries": [560, 1360], "rowBoundaries": [382, 698]},
        "slotPlan": {"slot0": "Control/SessionDescriptor", "slots1To30": "Transport"},
        "variants": list(VARIANTS),
        "variantTransforms": {
            "clean": "sequence41 canonical raster",
            "chroma-neutralized": "replace every BGR triple by rounded BT.709 luma",
            "base-neutralized": "replace top 128x64 phase checker by neutral 128",
            "fine-neutralized": "replace bottom 128x64 phase checker by neutral 128",
            "localized-stale": "copy sequence40 center timing patch and freshness-region-4 data tiles",
            "wrong-sequence": "copy every sequence40 data rectangle under sequence41 Bootstrap and pilots",
        },
        "independentRebuild": "tests/PBModulation/generate_unified_visual_cpu_golden.py --check",
        "rasterBlake3": digest_map,
    }
    return {
        "accepted-stream.bin": AcceptedStream(blocks),
        "bootstrap-sequence40.bin": previous_record,
        "bootstrap-sequence41.bin": current_record,
        "contract.json": (json.dumps(contract, indent=2, sort_keys=True) + "\n").encode(),
        "mixed-codewords.bin": codewords,
        "raster-digests.bin": digest_bytes,
    }


def CheckFiles(root: Path, expected: dict[str, bytes]) -> None:
    if not root.is_dir():
        raise SystemExit(f"Golden directory missing: {root}")
    entries = tuple(root.iterdir())
    actual_names = {path.name for path in entries}
    expected_names = set(expected)
    if actual_names != expected_names:
        raise SystemExit(f"Golden inventory mismatch: missing={sorted(expected_names - actual_names)}, "
                         f"extra={sorted(actual_names - expected_names)}")
    for name, content in expected.items():
        path = root / name
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"Golden entry is not a regular file: {name}")
        actual = path.read_bytes()
        if actual != content:
            raise SystemExit(f"Golden mismatch: {name}: expected={blake3(content).hexdigest()} "
                             f"actual={blake3(actual).hexdigest()}")
    print(f"UNIFIED_VISUAL_CPU_GOLDEN_CHECK_PASS files={len(expected)}")


def WriteFiles(root: Path, files: dict[str, bytes]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise SystemExit(f"Refusing to overwrite non-empty Golden directory: {root}")
    for name, content in files.items():
        with (root / name).open("xb") as output:
            output.write(content)
    print(f"UNIFIED_VISUAL_CPU_GOLDEN_WRITE_PASS files={len(files)}")


def Main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args()
    files = BuildFiles()
    if args.check:
        CheckFiles(args.output, files)
    else:
        WriteFiles(args.output, files)


if __name__ == "__main__":
    Main()
