#!/usr/bin/env python3
"""Independent deterministic Golden generator for PB-Unified-SC6-V3."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
from typing import Any

from blake3 import blake3


DEFAULT_ROOT = Path(__file__).resolve().parents[1] / "golden" / "unified-sc6"
MAPPING_STREAM_DOMAIN = b"PixelBridge.UnifiedSc6MappingStream.3\0"
ARTIFACT_DOMAIN = b"PixelBridge.UnifiedSc6MappingArtifact.2\0"
DATA_TILE_COUNT = 41872
MAPPING_VERSION = 3
MAPPING_SEQUENCE_PERIOD = 16
CODEWORD_BITS = 16200
LUMA_PLANES = 4
CHROMA_PLANES = 2
LUMA_DEFICIT_BITS = 3816
UNUSED_LUMA_BITS = 5488
UNUSED_CHROMA_BITS = 2744
MAPPING_CONTRACT_FLAGS = 0x00000007
CODEWORD_INTERLEAVE = (CODEWORD_BITS, 16067, 5603, 7919, 1009, MAPPING_SEQUENCE_PERIOD)
FRESHNESS_COLUMN_BOUNDARIES = (560, 1360)
FRESHNESS_ROW_BOUNDARIES = (382, 698)
FRESHNESS_TILE_COUNTS = (3850, 5836, 3850, 4291, 6506, 4291, 3773, 5702, 3773)
CHROMA_REGION_ORDER = (0, 1, 2, 3, 6, 8, 4, 5, 7)
DATA_REGIONS = (
    (96, 96, 1728, 60), (224, 160, 672, 126), (1024, 160, 672, 126),
    (96, 288, 1728, 186), (224, 476, 672, 126), (1024, 476, 672, 126),
    (96, 604, 1728, 186), (224, 792, 672, 126), (1024, 792, 672, 126),
    (96, 920, 1728, 60), (6, 96, 84, 888), (1830, 96, 84, 888))

MASKS_BY_LABEL = (
    0x0F6A100, 0x004A5BF, 0x00F731C, 0x0073C61,
    0x0F78E3F, 0x00218CE, 0x0319980, 0x033BDEF,
    0x1095EFF, 0x1FB5A40, 0x1F08CE3, 0x1F8C39E,
    0x10871C0, 0x1FDE731, 0x1CE667F, 0x1CC4210)
CHROMA_STATES_BY_LABEL = (
    (253, 127, 239, 0),
    (253, 174, 81, 1),
    (67, 193, 81, 2),
    (67, 146, 239, 3))
LANES = ((0, 145800), (1, 16200), (2, 81000))


def PrettyJson(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def ArtifactDigest(content: bytes) -> str:
    hasher = blake3()
    hasher.update(ARTIFACT_DOMAIN)
    hasher.update(content)
    return hasher.hexdigest()


def HasNoIsolatedCells(mask: int) -> bool:
    for index in range(25):
        value = (mask >> index) & 1
        x = index % 5
        y = index // 5
        neighbors = []
        if x > 0:
            neighbors.append(index - 1)
        if x < 4:
            neighbors.append(index + 1)
        if y > 0:
            neighbors.append(index - 5)
        if y < 4:
            neighbors.append(index + 5)
        if not any(((mask >> neighbor) & 1) == value for neighbor in neighbors):
            return False
    return True


def DataCoordinates() -> tuple[tuple[int, int], ...]:
    coordinates = []
    for x, y, width, height in DATA_REGIONS:
        for tile_y in range(y, y + height, 6):
            for tile_x in range(x, x + width, 6):
                coordinates.append((tile_x, tile_y))
    if len(coordinates) != DATA_TILE_COUNT:
        raise SystemExit(f"Independent Data tile count mismatch: {len(coordinates)}")
    return tuple(coordinates)


def FreshnessRegion(x: int, y: int) -> int:
    center_x, center_y = x + 3, y + 3
    column = 0 if center_x < FRESHNESS_COLUMN_BOUNDARIES[0] else \
        1 if center_x < FRESHNESS_COLUMN_BOUNDARIES[1] else 2
    row = 0 if center_y < FRESHNESS_ROW_BOUNDARIES[0] else \
        1 if center_y < FRESHNESS_ROW_BOUNDARIES[1] else 2
    return row * 3 + column


def BuildFreshnessCatalog() -> tuple[tuple[int, ...], ...]:
    grouped: list[list[int]] = [[] for _ in range(9)]
    for tile, (x, y) in enumerate(DataCoordinates()):
        grouped[FreshnessRegion(x, y)].append(tile)
    catalog = tuple(tuple(region) for region in grouped)
    counts = tuple(len(region) for region in catalog)
    if counts != FRESHNESS_TILE_COUNTS:
        raise SystemExit(f"Independent freshness tile counts mismatch: {counts}")
    return catalog


FRESHNESS_TILES = BuildFreshnessCatalog()


def PrimaryLumaBits(region: int) -> int:
    return min(len(FRESHNESS_TILES[region]) * LUMA_PLANES, CODEWORD_BITS)


def LumaDeficitPrefix(end_region: int) -> int:
    return sum(CODEWORD_BITS - PrimaryLumaBits(region) for region in range(end_region))


def RegionLumaSite(region: int, region_bit: int) -> tuple[int, int, int]:
    tile_rank, plane = divmod(region_bit, LUMA_PLANES)
    return 0, FRESHNESS_TILES[region][tile_rank], plane


def LumaSurplusSite(surplus_bit: int) -> tuple[int, int, int]:
    for region in range(9):
        surplus = max(len(FRESHNESS_TILES[region]) * LUMA_PLANES - CODEWORD_BITS, 0)
        if surplus_bit < surplus:
            return RegionLumaSite(region, CODEWORD_BITS + surplus_bit)
        surplus_bit -= surplus
    raise AssertionError("Luma surplus bit is outside the physical carrier")


def PermuteLaneCodewordBit(logical_bit: int, frame_sequence: int) -> int:
    modulus, multiplier, _, offset, phase_step, phase_count = CODEWORD_INTERLEAVE
    codeword_slot, codeword_bit = divmod(logical_bit, modulus)
    phase = frame_sequence % phase_count
    domain_bit = (codeword_bit * multiplier + offset + phase * phase_step) % modulus
    return codeword_slot * modulus + domain_bit


def InvertLaneCodewordBit(domain_logical_bit: int, frame_sequence: int) -> int:
    modulus, _, inverse, offset, phase_step, phase_count = CODEWORD_INTERLEAVE
    codeword_slot, domain_bit = divmod(domain_logical_bit, modulus)
    phase = frame_sequence % phase_count
    codeword_bit = ((domain_bit - offset - phase * phase_step) * inverse) % modulus
    return codeword_slot * modulus + codeword_bit


def MapLogicalBit(lane_id: int, logical_bit: int, frame_sequence: int) -> tuple[int, int, int]:
    lane, logical_bits = LANES[lane_id]
    assert lane == lane_id and 0 <= logical_bit < logical_bits
    assert frame_sequence >= 0
    domain_logical_bit = PermuteLaneCodewordBit(logical_bit, frame_sequence)
    if lane_id == 0:
        codeword_slot, codeword_bit = divmod(domain_logical_bit, CODEWORD_BITS)
        primary_bits = PrimaryLumaBits(codeword_slot)
        if codeword_bit < primary_bits:
            return RegionLumaSite(codeword_slot, codeword_bit)
        return LumaSurplusSite(LumaDeficitPrefix(codeword_slot) + codeword_bit - primary_bits)
    if lane_id == 1:
        return LumaSurplusSite(LUMA_DEFICIT_BITS + domain_logical_bit)
    remaining = domain_logical_bit
    for region in CHROMA_REGION_ORDER:
        capacity = len(FRESHNESS_TILES[region]) * CHROMA_PLANES
        if remaining < capacity:
            tile_rank, plane = divmod(remaining, CHROMA_PLANES)
            return 1, FRESHNESS_TILES[region][tile_rank], plane
        remaining -= capacity
    raise AssertionError("Chroma bit is outside the physical carrier")


def ValidateFrozenConstants() -> None:
    if any(not 8 <= mask.bit_count() <= 17 or not HasNoIsolatedCells(mask) for mask in MASKS_BY_LABEL):
        raise SystemExit("Independent codebook structure validation failed")
    if any((MASKS_BY_LABEL[label] ^ MASKS_BY_LABEL[label + 8]) != 0x1FFFFFF for label in range(8)):
        raise SystemExit("Independent codebook complement validation failed")
    minimum_hamming = min((left ^ right).bit_count() for index, left in enumerate(MASKS_BY_LABEL)
                          for right in MASKS_BY_LABEL[index + 1:])
    if minimum_hamming < 9:
        raise SystemExit(f"Independent codebook Hamming validation failed: {minimum_hamming}")
    if tuple(sorted(CHROMA_REGION_ORDER)) != tuple(range(9)):
        raise SystemExit("Independent Chroma region order is not a permutation")
    if tuple(logical_bits for _, logical_bits in LANES) != (145800, 16200, 81000):
        raise SystemExit("Independent lane capacity validation failed")
    modulus, multiplier, inverse, offset, phase_step, phase_count = CODEWORD_INTERLEAVE
    if modulus != CODEWORD_BITS or multiplier * inverse % modulus != 1 or not 0 <= offset < modulus or \
            not 0 < phase_step < modulus or phase_count != MAPPING_SEQUENCE_PERIOD:
        raise SystemExit("Independent codeword interleave validation failed")
    deficit_bits = sum(CODEWORD_BITS - PrimaryLumaBits(region) for region in range(9))
    surplus_bits = sum(max(len(FRESHNESS_TILES[region]) * LUMA_PLANES - CODEWORD_BITS, 0)
                       for region in range(9))
    if deficit_bits != LUMA_DEFICIT_BITS or surplus_bits != LUMA_DEFICIT_BITS + LANES[1][1] + UNUSED_LUMA_BITS:
        raise SystemExit("Independent Luma region-local capacity validation failed")
    if DATA_TILE_COUNT * CHROMA_PLANES != LANES[2][1] + UNUSED_CHROMA_BITS:
        raise SystemExit("Independent Chroma region-local capacity validation failed")


def MappingStreamDigest() -> str:
    ValidateFrozenConstants()
    luma_owners = bytearray(DATA_TILE_COUNT * LUMA_PLANES)
    chroma_owners = bytearray(DATA_TILE_COUNT * CHROMA_PLANES)
    hasher = blake3()
    hasher.update(MAPPING_STREAM_DOMAIN)
    for lane_id, lane in enumerate(LANES):
        for logical_bit in range(lane[1]):
            carrier, tile, plane = MapLogicalBit(lane_id, logical_bit, 0)
            owners = luma_owners if carrier == 0 else chroma_owners
            planes = LUMA_PLANES if carrier == 0 else CHROMA_PLANES
            site_index = tile * planes + plane
            if owners[site_index]:
                raise SystemExit(f"Independent mapping collision: lane={lane_id} logical={logical_bit}")
            owners[site_index] = 1
            hasher.update(struct.pack("<BBBBII", lane_id, carrier, plane, 0, logical_bit, tile))
            domain_logical_bit = PermuteLaneCodewordBit(logical_bit, 0)
            if InvertLaneCodewordBit(domain_logical_bit, 0) != logical_bit:
                raise SystemExit("Independent codeword interleave inverse mismatch")
    if sum(luma_owners) != LANES[0][1] + LANES[1][1] or sum(chroma_owners) != LANES[2][1]:
        raise SystemExit("Independent mapped-site count mismatch")
    if len(luma_owners) - sum(luma_owners) != UNUSED_LUMA_BITS or \
            len(chroma_owners) - sum(chroma_owners) != UNUSED_CHROMA_BITS:
        raise SystemExit("Independent reserved-site count mismatch")
    return hasher.hexdigest()


def BuildFiles() -> dict[str, bytes]:
    codebook = struct.pack("<16I", *MASKS_BY_LABEL)
    chroma_states = bytes(value for state in CHROMA_STATES_BY_LABEL for value in state)
    mapping_contract = bytearray(b"PBUSC6M3")
    mapping_contract += struct.pack("<IIIIII", MAPPING_VERSION, DATA_TILE_COUNT, 9, LUMA_PLANES,
                                    CHROMA_PLANES, MAPPING_SEQUENCE_PERIOD)
    mapping_contract += struct.pack("<9I", *FRESHNESS_TILE_COUNTS)
    mapping_contract += struct.pack("<2I", *FRESHNESS_COLUMN_BOUNDARIES)
    mapping_contract += struct.pack("<2I", *FRESHNESS_ROW_BOUNDARIES)
    mapping_contract += bytes(CHROMA_REGION_ORDER) + bytes(3)
    mapping_contract += struct.pack("<3I", *(logical_bits for _, logical_bits in LANES))
    mapping_contract += struct.pack("<IIIII", CODEWORD_BITS, LUMA_DEFICIT_BITS, UNUSED_LUMA_BITS,
                                    UNUSED_CHROMA_BITS, MAPPING_CONTRACT_FLAGS)
    mapping_contract += struct.pack("<6I", *CODEWORD_INTERLEAVE)
    mapping_contract += bytes(8)
    assert len(mapping_contract) == 160
    stream_digest = MappingStreamDigest()
    files = {
        "chroma-states.bin": chroma_states,
        "codebook.bin": codebook,
        "mapping-contract.bin": bytes(mapping_contract),
        "mapping-stream.blake3": (stream_digest + "\n").encode("ascii"),
    }
    manifest = {
        "schema": "PixelBridge.UnifiedSc6MappingGolden.2",
        "status": "DeterministicIndependentContract",
        "profile": {"name": "PB-Unified-SC6-V3", "profileIdHex": "5042554e49534333", "layoutVersion": 10},
        "cell": {"tilePixels": 6, "glyphPixels": 5, "separatorPixels": 1, "minimumMaskHammingDistance": 9},
        "regionLocalization": {
            "freshnessTileCounts": FRESHNESS_TILE_COUNTS,
            "basePrimaryRegionByCodeword": list(range(9)),
            "chromaRegionOrder": CHROMA_REGION_ORDER,
            "sequencePeriod": MAPPING_SEQUENCE_PERIOD,
            "codewordInterleave": {
                "modulus": CODEWORD_INTERLEAVE[0], "multiplier": CODEWORD_INTERLEAVE[1],
                "inverse": CODEWORD_INTERLEAVE[2], "offset": CODEWORD_INTERLEAVE[3],
                "phaseStep": CODEWORD_INTERLEAVE[4], "phaseCount": CODEWORD_INTERLEAVE[5],
            },
        },
        "mappingStreamFrameSequenceZeroBlake3": stream_digest,
        "mappingStreamFormat": "BLAKE3(domain || repeated LE lane/carrier/plane/reserved/logical/tile records)",
        "independentRebuild": "tests/PBModulation/generate_unified_sc6_mapping_golden.py --check",
        "files": [{"file": name, "bytes": len(content), "domainBlake3": ArtifactDigest(content)}
                  for name, content in sorted(files.items())],
    }
    files["manifest.json"] = PrettyJson(manifest)
    return files


def CheckFiles(root: Path, expected: dict[str, bytes]) -> None:
    if not root.is_dir() or root.is_symlink():
        raise SystemExit(f"Golden directory missing or unsafe: {root}")
    actual_names = {path.name for path in root.iterdir()}
    if actual_names != set(expected):
        raise SystemExit(f"Golden inventory mismatch: expected={sorted(expected)} actual={sorted(actual_names)}")
    for name, content in expected.items():
        path = root / name
        if not path.is_file() or path.is_symlink() or path.read_bytes() != content:
            raise SystemExit(f"Golden mismatch: {name}")


def WriteFiles(root: Path, files: dict[str, bytes]) -> None:
    if root.exists():
        raise SystemExit(f"Refusing existing output directory: {root}")
    root.mkdir(parents=True)
    for name, content in sorted(files.items()):
        with (root / name).open("xb") as output:
            output.write(content)


def Main() -> None:
    parser = argparse.ArgumentParser(description="Independently rebuild the Unified SC6 V3 mapping Golden")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--golden-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    files = BuildFiles()
    if args.check:
        if args.output_dir is not None:
            parser.error("--check cannot be combined with --output-dir")
        CheckFiles(args.golden_root, files)
    else:
        if args.output_dir is None:
            parser.error("generation requires --output-dir")
        WriteFiles(args.output_dir, files)
    print(f"UNIFIED_SC6_INDEPENDENT_GOLDEN_PASS files={len(files)} mapping_stream_blake3={MappingStreamDigest()}")


if __name__ == "__main__":
    Main()
