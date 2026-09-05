#!/usr/bin/env python3
"""Independent deterministic Golden generator for PB-Unified-SC6-V2."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
from typing import Any

from blake3 import blake3


DEFAULT_ROOT = Path(__file__).resolve().parents[1] / "golden" / "unified-sc6"
MAPPING_STREAM_DOMAIN = b"PixelBridge.UnifiedSc6MappingStream.2\0"
ARTIFACT_DOMAIN = b"PixelBridge.UnifiedSc6MappingArtifact.1\0"
DATA_TILE_COUNT = 41872
BASE_DEDICATED_BITS = 125616
BASE_SHARED_BITS = 20184
FINE_FIRST_POSITION = 20184
FINE_END_POSITION = 36384
CHROMA_USED_TILES = 40500

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
PLANE3_ORDER = (41872, 31439, 6879, 34120)
CHROMA_ORDER = (41872, 23661, 41557, 9489)
LANES = (
    (0, 145800, 48467, 38003, 17811, 84229, 16, 3),
    (1, 16200, 2159, 14039, 4198, 9953, 16, 14),
    (2, 81000, 71357, 67493, 71539, 50837, 16, 15))


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


def ValidateFrozenConstants() -> None:
    if any(not 8 <= mask.bit_count() <= 17 or not HasNoIsolatedCells(mask) for mask in MASKS_BY_LABEL):
        raise SystemExit("Independent codebook structure validation failed")
    if any((MASKS_BY_LABEL[label] ^ MASKS_BY_LABEL[label + 8]) != 0x1FFFFFF for label in range(8)):
        raise SystemExit("Independent codebook complement validation failed")
    minimum_hamming = min((left ^ right).bit_count() for index, left in enumerate(MASKS_BY_LABEL)
                          for right in MASKS_BY_LABEL[index + 1:])
    if minimum_hamming < 9:
        raise SystemExit(f"Independent codebook Hamming validation failed: {minimum_hamming}")
    for modulus, multiplier, inverse, offset in (PLANE3_ORDER, CHROMA_ORDER):
        if modulus != DATA_TILE_COUNT or multiplier * inverse % modulus != 1 or not 0 <= offset < modulus:
            raise SystemExit("Independent tile permutation validation failed")
    expected_bits = (145800, 16200, 81000)
    for lane_id, lane in enumerate(LANES):
        _, logical_bits, multiplier, inverse, offset, phase_step, phase_count, sequence_offset = lane
        if logical_bits != expected_bits[lane_id] or multiplier * inverse % logical_bits != 1 or \
                math.gcd(phase_step, logical_bits) != 1 or not 0 <= offset < logical_bits or \
                phase_count != 16 or not 0 <= sequence_offset < phase_count:
            raise SystemExit(f"Independent lane interleave validation failed: lane={lane_id}")
    if BASE_DEDICATED_BITS + BASE_SHARED_BITS != 145800 or FINE_FIRST_POSITION != BASE_SHARED_BITS or \
            FINE_END_POSITION != FINE_FIRST_POSITION + 16200 or CHROMA_USED_TILES * 2 != 81000:
        raise SystemExit("Independent lane ownership validation failed")


def MapLogicalBit(lane_id: int, logical_bit: int, frame_sequence: int) -> tuple[int, int, int]:
    lane, logical_bits, multiplier, _, offset, phase_step, phase_count, sequence_offset = LANES[lane_id]
    assert lane == lane_id and 0 <= logical_bit < logical_bits
    phase = (frame_sequence % phase_count + sequence_offset) % phase_count
    domain_bit = (logical_bit * multiplier + offset + phase * phase_step) % logical_bits
    if lane_id == 0:
        if domain_bit < BASE_DEDICATED_BITS:
            return 0, domain_bit // 3, domain_bit % 3
        position = domain_bit - BASE_DEDICATED_BITS
        return 0, (position * PLANE3_ORDER[1] + PLANE3_ORDER[3]) % DATA_TILE_COUNT, 3
    if lane_id == 1:
        position = domain_bit + FINE_FIRST_POSITION
        return 0, (position * PLANE3_ORDER[1] + PLANE3_ORDER[3]) % DATA_TILE_COUNT, 3
    position = domain_bit // 2
    return 1, (position * CHROMA_ORDER[1] + CHROMA_ORDER[3]) % DATA_TILE_COUNT, domain_bit % 2


def MappingStreamDigest() -> str:
    ValidateFrozenConstants()
    luma_owners = bytearray(DATA_TILE_COUNT * 4)
    chroma_owners = bytearray(DATA_TILE_COUNT * 2)
    hasher = blake3()
    hasher.update(MAPPING_STREAM_DOMAIN)
    for lane_id, lane in enumerate(LANES):
        for logical_bit in range(lane[1]):
            carrier, tile, plane = MapLogicalBit(lane_id, logical_bit, 0)
            owners = luma_owners if carrier == 0 else chroma_owners
            site_index = tile * (4 if carrier == 0 else 2) + plane
            if owners[site_index]:
                raise SystemExit(f"Independent mapping collision: lane={lane_id} logical={logical_bit}")
            owners[site_index] = 1
            hasher.update(struct.pack("<BBBBII", lane_id, carrier, plane, 0, logical_bit, tile))
    if sum(luma_owners) != 162000 or sum(chroma_owners) != 81000:
        raise SystemExit("Independent mapped-site count mismatch")
    if len(luma_owners) - sum(luma_owners) != 5488 or len(chroma_owners) - sum(chroma_owners) != 2744:
        raise SystemExit("Independent reserved-site count mismatch")
    return hasher.hexdigest()


def BuildFiles() -> dict[str, bytes]:
    codebook = struct.pack("<16I", *MASKS_BY_LABEL)
    chroma_states = bytes(value for state in CHROMA_STATES_BY_LABEL for value in state)
    mapping = bytearray(b"PBUSC6M2")
    mapping += struct.pack("<IIIIII", 2, DATA_TILE_COUNT, PLANE3_ORDER[1], PLANE3_ORDER[3],
                           CHROMA_ORDER[1], CHROMA_ORDER[3])
    for lane in LANES:
        mapping += struct.pack("<IIIIIIII", *lane)
    stream_digest = MappingStreamDigest()
    files = {
        "chroma-states.bin": chroma_states,
        "codebook.bin": codebook,
        "mapping-contract.bin": bytes(mapping),
        "mapping-stream.blake3": (stream_digest + "\n").encode("ascii"),
    }
    manifest = {
        "schema": "PixelBridge.UnifiedSc6MappingGolden.1",
        "status": "DeterministicIndependentContract",
        "profile": {"name": "PB-Unified-SC6-V2", "profileIdHex": "5042554e49534332", "layoutVersion": 9},
        "cell": {"tilePixels": 6, "glyphPixels": 5, "separatorPixels": 1, "minimumMaskHammingDistance": 9},
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
    parser = argparse.ArgumentParser(description="Independently rebuild the Unified SC6 mapping Golden")
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
