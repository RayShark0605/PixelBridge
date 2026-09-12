#!/usr/bin/env python3
"""Independent PB-Experimental-GrayStates-1 CPU-raster oracle Golden.

The gray-state identity (layout 12) shares the frozen SC6-V3 mapping, codebook,
interleave, slot plan and scaffold byte-for-byte; only the four foreground
states and the calibration state stripes change to gray luma levels. This
generator imports only earlier independent Python oracles (never C++ headers,
executables or build artifacts), overrides the state table and wire identity,
and replaces the chroma-neutralized digest variant - a no-op on gray rasters -
with a state-collapsed variant that freezes the fail-closed pilot boundary.
Generation is create-only; --check is byte-for-byte and validates exact
inventory.
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


DEFAULT_ROOT = Path(__file__).resolve().parents[1] / "golden" / "unified-graystates-cpu-oracle"
PROFILE_ID = 0x5042475953544131
LAYOUT_VERSION = 12
PROFILE_NAME = "PB-Experimental-GrayStates-1"
LOW, HIGH, NEUTRAL = 8, 160, 128
LUMA_LEVELS = (8, 64, 160, 232)
CALIBRATION = ((736, 16), (1696, 16), (96, 1000), (1056, 1000))
PHASE = ((896, 16), (896, 1000))
# Gray foreground levels replace the SC6-V3 iso-luma chroma colors; spacing is
# 56, comfortably above the decode policy's minimum luma level gap of 32.
GRAY_STATES_BY_LABEL = (
    (56, 56, 56, 0),
    (112, 112, 112, 1),
    (168, 168, 168, 2),
    (248, 248, 248, 3))
VARIANTS = ("clean", "state-collapsed", "base-neutralized", "fine-neutralized",
            "localized-stale", "wrong-sequence")


def Record(sequence: int) -> bytes:
    prefix = struct.pack("<4sBBBBQQQII", b"PBRG", 1, 1, 0, LAYOUT_VERSION, PROFILE_ID,
                         sc6cpu.SESSION_TAG, sequence, 0x21222324, 0)
    assert len(prefix) == 40
    import generate_local_desktop_golden as scaffold
    return prefix + struct.pack("<I", scaffold.Crc32C(prefix))


def Raster(sequence: int, record: bytes, codewords: bytes) -> bytes:
    import generate_local_desktop_golden as scaffold
    image = bytearray(scaffold.Raster(scaffold.EncodeRs(record), scaffold.TimingBits(record),
                                      scaffold.MarkerBits()))
    for x, y in CALIBRATION:
        for label, level in enumerate(LUMA_LEVELS):
            sc6cpu.Fill(image, x + label * 32, y, 32, 24, level)
        sc6cpu.Fill(image, x, y + 24, 128, 8, NEUTRAL)
        for label, (level, _, _, state_label) in enumerate(GRAY_STATES_BY_LABEL):
            assert label == state_label
            sc6cpu.Fill(image, x + label * 32, y + 32, 32, 32, level)
    for fine, (x, y) in enumerate(PHASE):
        for tile in range((128 // 6) * (64 // 6)):
            tile_x = x + (tile % 21) * 6
            tile_y = y + (tile // 21) * 6
            mask = mapping.MASKS_BY_LABEL[sc6cpu.PhaseLabel(bool(fine), tile, sequence)]
            for row in range(6):
                for column in range(6):
                    foreground = row < 5 and column < 5 and mask & (1 << (row * 5 + column))
                    sc6cpu.Fill(image, tile_x + column, tile_y + row, 1, 1, HIGH if foreground else LOW)

    luma_labels = bytearray(mapping.DATA_TILE_COUNT)
    chroma_labels = bytearray(mapping.DATA_TILE_COUNT)
    lane_starts = (0, 9 * 16200, 10 * 16200)
    for lane, lane_contract in enumerate(mapping.LANES):
        for logical_bit in range(lane_contract[1]):
            carrier, tile, plane = mapping.MapLogicalBit(lane, logical_bit, sequence)
            global_bit = lane_starts[lane] + logical_bit
            value = (codewords[global_bit // 8] >> (global_bit % 8)) & 1
            if carrier == 0:
                luma_labels[tile] |= value << plane
            else:
                chroma_labels[tile] |= value << plane
    coordinates = sc6cpu.DataCoordinates()
    for tile, (x, y) in enumerate(coordinates):
        mask = mapping.MASKS_BY_LABEL[luma_labels[tile]]
        level = GRAY_STATES_BY_LABEL[chroma_labels[tile]][0]
        for row in range(6):
            for column in range(6):
                foreground = row < 5 and column < 5 and mask & (1 << (row * 5 + column))
                sc6cpu.Fill(image, x + column, y + row, 1, 1, level if foreground else LOW)
    return bytes(image)


def CollapseStateStripes(image: bytearray) -> None:
    for x, y in CALIBRATION:
        sc6cpu.Fill(image, x, y + 32, 128, 32, NEUTRAL)


def RasterDigests(previous: bytes, current: bytes) -> tuple[bytes, dict[str, str]]:
    variants: list[bytes] = [current]
    state_collapsed = bytearray(current)
    CollapseStateStripes(state_collapsed)
    variants.append(bytes(state_collapsed))
    base_neutral = bytearray(current)
    sc6cpu.Fill(base_neutral, 896, 16, 128, 64, NEUTRAL)
    variants.append(bytes(base_neutral))
    fine_neutral = bytearray(current)
    sc6cpu.Fill(fine_neutral, 896, 1000, 128, 64, NEUTRAL)
    variants.append(bytes(fine_neutral))
    localized_stale = bytearray(current)
    sc6cpu.CopyRegion(previous, localized_stale, (896, 476, 128, 128))
    sc6cpu.CopyFreshnessData(previous, localized_stale, 4)
    variants.append(bytes(localized_stale))
    wrong_sequence = bytearray(current)
    for region in sc6cpu.DATA_REGIONS:
        sc6cpu.CopyRegion(previous, wrong_sequence, region)
    variants.append(bytes(wrong_sequence))
    assert len(variants) == len(VARIANTS)
    digests = tuple(blake3(value).digest() for value in variants)
    return b"".join(digests), {name: digest.hex() for name, digest in zip(VARIANTS, digests, strict=True)}


def BuildFiles() -> dict[str, bytes]:
    mapping.ValidateFrozenConstants()
    for level, _, _, state_label in GRAY_STATES_BY_LABEL:
        assert 0 < level < 256
        assert state_label == GRAY_STATES_BY_LABEL.index((level, level, level, state_label))
    previous_record = Record(40)
    current_record = Record(41)
    codewords, blocks = sc6cpu.MixedCodewords(41)
    previous = Raster(40, previous_record, codewords)
    current = Raster(41, current_record, codewords)
    digest_bytes, digest_map = RasterDigests(previous, current)
    contract = {
        "artifact": f"{PROFILE_NAME} CPU oracle Golden",
        "canvas": {"width": 1920, "height": 1080, "pixelFormat": "BGRA8_UNORM_SDR"},
        "codewords": {"count": 15, "bytesEach": 2025, "informationBytesEach": 1350,
                      "innerFecProfile": "DVB-S2-Short-N16200-K10800"},
        "data": {"lowLuma": LOW, "highLuma": HIGH, "neutralLuma": NEUTRAL,
                 "tiles": mapping.DATA_TILE_COUNT, "tilePixels": 6, "glyphPixels": 5,
                 "separatorPixels": 1, "foregroundStates": "gray", "grayLevels": [56, 112, 168, 248]},
        "pilots": {"lumaRows": 24, "neutralRows": 8, "stateRows": 32,
                   "lumaLevels": LUMA_LEVELS, "stateStripes": "gray"},
        "freshnessPartition": {"columnBoundaries": [560, 1360], "rowBoundaries": [382, 698]},
        "profile": {"name": PROFILE_NAME, "profileIdHex": f"{PROFILE_ID:016x}", "layoutVersion": LAYOUT_VERSION},
        "slotPlan": {"slot0": "Control/SessionDescriptor", "slots1To14": "Transport"},
        "variants": list(VARIANTS),
        "variantTransforms": {
            "clean": "sequence41 canonical raster",
            "state-collapsed": "replace the four gray state stripes of every calibration pilot by neutral 128",
            "base-neutralized": "replace top 128x64 phase checker by neutral 128",
            "fine-neutralized": "replace bottom 128x64 phase checker by neutral 128",
            "localized-stale": "copy sequence40 center timing patch and freshness-region-4 data tiles",
            "wrong-sequence": "copy every sequence40 data rectangle under sequence41 Bootstrap and pilots",
        },
        "independentRebuild": "tests/PBModulation/generate_unified_graystates_golden.py --check",
        "rasterBlake3": digest_map,
    }
    return {
        "accepted-stream.bin": sc6cpu.AcceptedStream(blocks),
        "bootstrap-sequence40.bin": previous_record,
        "bootstrap-sequence41.bin": current_record,
        "contract.json": (json.dumps(contract, indent=2, sort_keys=True) + "\n").encode(),
        "mixed-codewords.bin": codewords,
        "raster-digests.bin": digest_bytes,
    }


def CheckFiles(root: Path, expected: dict[str, bytes]) -> None:
    actual_names = sorted(path.name for path in root.iterdir() if path.is_file())
    if actual_names != sorted(expected):
        raise SystemExit(f"golden inventory mismatch: {actual_names} != {sorted(expected)}")
    for name, content in expected.items():
        actual = (root / name).read_bytes()
        if actual != content:
            raise SystemExit(f"golden mismatch for {name}")


def WriteFiles(root: Path, files: dict[str, bytes]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        (root / name).write_bytes(content)


def Main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    files = BuildFiles()
    if arguments.check:
        CheckFiles(arguments.golden_root, files)
        print(f"GRAYSTATES_GOLDEN_CHECK_OK {arguments.golden_root}")
        return
    WriteFiles(arguments.golden_root, files)
    print(f"GRAYSTATES_GOLDEN_WRITTEN {arguments.golden_root}")


if __name__ == "__main__":
    Main()
