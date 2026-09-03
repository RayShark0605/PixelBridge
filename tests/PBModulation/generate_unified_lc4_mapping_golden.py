from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import sys
from typing import Any

from blake3 import blake3


sys.dont_write_bytecode = True

DEFAULT_ROOT = Path(__file__).resolve().parents[1] / "golden" / "unified-lc4"
DATASET_MANIFEST_BLAKE3 = "a69fa7980680549ed951d3ddda7ab2e48a8da893a59180ccbb7bd83a2ac65ed3"
MAPPING_STREAM_DOMAIN = b"PixelBridge.UnifiedLc4MappingStream.1\0"
ARTIFACT_DOMAIN = b"PixelBridge.UnifiedLc4MappingArtifact.1\0"
DATA_TILE_COUNT = 86688
BASE_DEDICATED_BITS = 260064
BASE_SHARED_BITS = 15336
FINE_FIRST_POSITION = 15336
FINE_END_POSITION = 80136
CHROMA_USED_TILES = 81000

MASKS_BY_LABEL = (
    0x07DC, 0x08EF, 0x1337, 0x6666, 0x34D3, 0x3BE0, 0x62B9, 0x718E,
    0xF823, 0xF710, 0xECC8, 0x9999, 0xCB2C, 0xC41F, 0x9D46, 0x8E71)
CHROMA_STATES_BY_LABEL = (
    (-24, 7, -16, 0),
    (24, 2, -16, 1),
    (-24, -2, 16, 2),
    (24, -7, 16, 3))
PLANE3_ORDER = (86688, 31439, 15215, 76408)
CHROMA_ORDER = (86688, 65533, 14965, 51321)
LANES = (
    (0, 275400, 194267, 200003, 163611, 230029, 16, 3),
    (1, 64800, 34559, 30239, 52798, 26153, 16, 14),
    (2, 162000, 152357, 148493, 152539, 50837, 16, 15))


def CanonicalJson(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")


def PrettyJson(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def Blake3(content: bytes) -> str:
    return blake3(content).hexdigest()


def ArtifactDigest(content: bytes) -> str:
    hasher = blake3()
    hasher.update(ARTIFACT_DOMAIN)
    hasher.update(content)
    return hasher.hexdigest()


def HasNoIsolatedCells(mask: int) -> bool:
    for index in range(16):
        value = (mask >> index) & 1
        x = index % 4
        y = index // 4
        neighbors = []
        if x > 0:
            neighbors.append(index - 1)
        if x < 3:
            neighbors.append(index + 1)
        if y > 0:
            neighbors.append(index - 4)
        if y < 3:
            neighbors.append(index + 4)
        if not any(((mask >> neighbor) & 1) == value for neighbor in neighbors):
            return False
    return True


def MapLogicalBit(lane_id: int, logical_bit: int, frame_sequence: int) -> tuple[int, int, int]:
    lane, logical_bits, multiplier, _, offset, phase_step, phase_count, sequence_offset = LANES[lane_id]
    assert lane == lane_id and 0 <= logical_bit < logical_bits
    phase = (frame_sequence % phase_count + sequence_offset) % phase_count
    domain_bit = (logical_bit * multiplier + offset + phase * phase_step) % logical_bits
    if lane_id == 0:
        if domain_bit < BASE_DEDICATED_BITS:
            return 0, domain_bit // 3, domain_bit % 3
        plane3_position = domain_bit - BASE_DEDICATED_BITS
        tile = (plane3_position * PLANE3_ORDER[1] + PLANE3_ORDER[3]) % DATA_TILE_COUNT
        return 0, tile, 3
    if lane_id == 1:
        plane3_position = domain_bit + FINE_FIRST_POSITION
        tile = (plane3_position * PLANE3_ORDER[1] + PLANE3_ORDER[3]) % DATA_TILE_COUNT
        return 0, tile, 3
    used_tile_position = domain_bit // 2
    tile = (used_tile_position * CHROMA_ORDER[1] + CHROMA_ORDER[3]) % DATA_TILE_COUNT
    return 1, tile, domain_bit % 2


def ValidateFrozenConstants() -> None:
    if any(mask.bit_count() != 8 or not HasNoIsolatedCells(mask) for mask in MASKS_BY_LABEL):
        raise SystemExit("Independent codebook balance/structure validation failed")
    if any((MASKS_BY_LABEL[label] ^ MASKS_BY_LABEL[label + 8]) != 0xFFFF for label in range(8)):
        raise SystemExit("Independent codebook complement validation failed")
    minimum_hamming = min((left ^ right).bit_count() for index, left in enumerate(MASKS_BY_LABEL)
                          for right in MASKS_BY_LABEL[index + 1:])
    if minimum_hamming < 8:
        raise SystemExit(f"Independent codebook Hamming validation failed: {minimum_hamming}")
    for modulus, multiplier, inverse, offset in (PLANE3_ORDER, CHROMA_ORDER):
        if modulus != DATA_TILE_COUNT or multiplier * inverse % modulus != 1 or not 0 <= offset < modulus:
            raise SystemExit("Independent tile permutation validation failed")
    expected_bits = (275400, 64800, 162000)
    for lane_id, lane in enumerate(LANES):
        _, logical_bits, multiplier, inverse, offset, phase_step, phase_count, sequence_offset = lane
        if logical_bits != expected_bits[lane_id] or multiplier * inverse % logical_bits != 1 or \
                math.gcd(phase_step, logical_bits) != 1 or not 0 <= offset < logical_bits or \
                phase_count != 16 or not 0 <= sequence_offset < phase_count:
            raise SystemExit(f"Independent lane interleave validation failed: lane={lane_id}")
    if BASE_DEDICATED_BITS + BASE_SHARED_BITS != 275400 or FINE_FIRST_POSITION != BASE_SHARED_BITS or \
            FINE_END_POSITION != FINE_FIRST_POSITION + 64800 or CHROMA_USED_TILES * 2 != 162000:
        raise SystemExit("Independent lane ownership validation failed")


def MappingStreamDigest() -> str:
    ValidateFrozenConstants()
    luma_owners = bytearray(DATA_TILE_COUNT * 4)
    chroma_owners = bytearray(DATA_TILE_COUNT * 2)
    hasher = blake3()
    hasher.update(MAPPING_STREAM_DOMAIN)
    for lane_id, lane in enumerate(LANES):
        logical_bits = lane[1]
        for logical_bit in range(logical_bits):
            carrier, tile, plane = MapLogicalBit(lane_id, logical_bit, 0)
            ownership = luma_owners if carrier == 0 else chroma_owners
            site_index = tile * (4 if carrier == 0 else 2) + plane
            if ownership[site_index]:
                raise SystemExit(f"Independent mapping collision: lane={lane_id} logical={logical_bit}")
            ownership[site_index] = 1
            hasher.update(struct.pack("<BBBBII", lane_id, carrier, plane, 0, logical_bit, tile))
    if sum(luma_owners) != 340200 or sum(chroma_owners) != 162000:
        raise SystemExit("Independent mapped-site count mismatch")
    if len(luma_owners) - sum(luma_owners) != 6552 or len(chroma_owners) - sum(chroma_owners) != 11376:
        raise SystemExit("Independent reserved-site count mismatch")
    return hasher.hexdigest()


def CodebookBinary() -> bytes:
    ValidateFrozenConstants()
    return struct.pack("<16H", *MASKS_BY_LABEL)


def ChromaStatesBinary() -> bytes:
    ValidateFrozenConstants()
    return b"".join(struct.pack("<hhhB", *state) for state in CHROMA_STATES_BY_LABEL)


def MappingBinary() -> bytes:
    ValidateFrozenConstants()
    output = bytearray(b"PBULC4M1")
    output += struct.pack("<IIIIII", 1, DATA_TILE_COUNT, PLANE3_ORDER[1], PLANE3_ORDER[3],
                          CHROMA_ORDER[1], CHROMA_ORDER[3])
    for lane in LANES:
        output += struct.pack("<IIIIIIII", *lane)
    return bytes(output)


def ExpectedMappingJson() -> dict[str, Any]:
    return {
        "candidateIndex": 1,
        "plane3TileOrder": {
            "multiplier": PLANE3_ORDER[1], "inverse": PLANE3_ORDER[2],
            "offset": PLANE3_ORDER[3], "modulus": PLANE3_ORDER[0]},
        "chromaTileOrder": {
            "multiplier": CHROMA_ORDER[1], "inverse": CHROMA_ORDER[2],
            "offset": CHROMA_ORDER[3], "modulus": CHROMA_ORDER[0], "usedTiles": CHROMA_USED_TILES},
        "lanes": [
            {
                "laneId": lane[0], "logicalBits": lane[1], "multiplier": lane[2], "inverse": lane[3],
                "offset": lane[4], "phaseStep": lane[5], "phaseCount": lane[6], "sequenceOffset": lane[7],
            }
            for lane in LANES],
        "lumaOwnership": {
            "baseDedicatedPlanes": [0, 1, 2],
            "baseDedicatedBits": BASE_DEDICATED_BITS,
            "basePlane3Positions": [0, BASE_SHARED_BITS],
            "finePlane3Positions": [FINE_FIRST_POSITION, FINE_END_POSITION],
            "reservedPlane3Positions": [FINE_END_POSITION, DATA_TILE_COUNT],
        },
        "phaseEvaluation": [0, 7, 15],
    }


def RequireRegularFile(path: Path) -> None:
    if not path.is_file() or path.is_symlink():
        raise SystemExit(f"Required regular file missing or unsafe: {path}")


def ValidateDataset(dataset: dict[str, Any]) -> None:
    core = dict(dataset)
    recorded_digest = core.pop("manifestBlake3", None)
    hasher = blake3()
    hasher.update(b"PixelBridge.UnifiedLc4DatasetManifestCore.1\0")
    hasher.update(CanonicalJson(core))
    samples = dataset.get("samples", [])
    counts = {split: sum(sample.get("split") == split for sample in samples)
              for split in ("Train", "Validation", "Holdout")}
    scales = {(sample["scale"]["numerator"], sample["scale"]["denominator"]) for sample in samples}
    source_digests = {source.get("id"): source.get("sourceBlake3") for source in dataset.get("historicalSources", [])}
    transform_counts = {
        "neutral": sum(bool(sample["transforms"]["neutralChroma"]) for sample in samples),
        "chroma420": sum(bool(sample["transforms"]["chroma420"]) for sample in samples),
        "blur": sum(int(sample["transforms"]["gaussianBlurPasses"]) > 0 for sample in samples),
        "quantization": sum(int(sample["transforms"]["quantizationBitsPerChannel"]) < 8 for sample in samples),
        "fractional": sum(any(int(value) != 0 for value in sample["transforms"]["originQ8"]) for sample in samples),
        "letterbox": sum(bool(sample["transforms"]["centeredLetterbox"]) for sample in samples),
    }
    for sample in samples:
        descriptor = {key: value for key, value in sample.items()
                      if key not in {"sampleBlake3", "split", "splitRankWithinStratum"}}
        sample_hasher = blake3()
        sample_hasher.update(b"PixelBridge.UnifiedLc4DatasetSample.1\0")
        sample_hasher.update(CanonicalJson(descriptor))
        if sample_hasher.hexdigest() != sample.get("sampleBlake3"):
            raise SystemExit("Independent dataset sample digest validation failed")
    if recorded_digest != DATASET_MANIFEST_BLAKE3 or hasher.hexdigest() != recorded_digest or len(samples) != 60 or \
            counts != {"Train": 30, "Validation": 20, "Holdout": 10} or dataset.get("sampleCounts") != counts or \
            scales != {(3, 4), (17, 20), (1, 1), (3, 2), (2, 1)} or \
            source_digests != {
                "historical-lf4": "a6e8947c391ccb817c6c4ec312de2a0dafdca2dd09b2a0520387912bf6fd14ad",
                "historical-shape-chroma": "612f40e97f30537379a1ad8bd75b5edc306d95d0f59bf17222d109022fa0004e"} or \
            any(count == 0 for count in transform_counts.values()):
        raise SystemExit("Independent dataset seal validation failed")


def LoadReports(root: Path) -> tuple[bytes, bytes, bytes, dict[str, Any], dict[str, Any], dict[str, Any]]:
    paths = [root / name for name in ("dataset-manifest.json", "selection-report.json", "holdout-report.json")]
    for path in paths:
        RequireRegularFile(path)
    dataset_bytes, selection_bytes, holdout_bytes = (path.read_bytes() for path in paths)
    dataset = json.loads(dataset_bytes.decode("utf-8"))
    selection = json.loads(selection_bytes.decode("utf-8"))
    holdout = json.loads(holdout_bytes.decode("utf-8"))
    if dataset.get("schema") != "PixelBridge.UnifiedLc4DatasetManifest.1" or dataset.get("status") != "SealedBeforeSearch":
        raise SystemExit("Independent dataset seal validation failed")
    ValidateDataset(dataset)
    if selection.get("schema") != "PixelBridge.UnifiedLc4SelectionReport.1" or \
            selection.get("status") != "ValidationWinnerFrozenBeforeHoldout" or \
            selection.get("datasetManifestBlake3") != DATASET_MANIFEST_BLAKE3 or \
            selection.get("splitDiscipline", {}).get("holdoutSamplesEvaluated") != 0 or \
            selection.get("splitDiscipline", {}).get("holdoutResultsOpened") is not False:
        raise SystemExit("Independent Validation selection discipline validation failed")
    winner = selection["winner"]
    if tuple(int(value, 16) for value in winner["masksHexByLabel"]) != MASKS_BY_LABEL or \
            tuple(tuple(value) for value in winner["chromaStatesByLabel"]) != CHROMA_STATES_BY_LABEL or \
            winner["mapping"] != ExpectedMappingJson() or \
            winner["mappingStreamFrameSequenceZeroBlake3"] != MappingStreamDigest() or \
            any(winner["validation"]["falseAccepted"].get(lane) != 0 for lane in ("Base", "Fine", "Chroma")) or \
            any(int(value) <= 0 for value in winner["validation"]["scores"].values()):
        raise SystemExit("Independent winner constants differ from the frozen mapping")
    if holdout.get("schema") != "PixelBridge.UnifiedLc4HoldoutReport.1" or holdout.get("status") != "Passed" or \
            holdout.get("datasetManifestBlake3") != DATASET_MANIFEST_BLAKE3 or \
            holdout.get("selectionReportBlake3") != Blake3(selection_bytes) or \
            any(holdout.get("evaluation", {}).get("falseAccepted", {}).get(lane) != 0
                for lane in ("Base", "Fine", "Chroma")) or \
            any(int(value) <= 0 for value in holdout.get("evaluation", {}).get("scores", {}).values()) or \
            holdout.get("holdoutPolicy") != {
                "failureAction": "block-without-retuning", "providerMetadataUsed": False,
                "singleEvaluation": True, "winnerOnly": True} or \
            holdout.get("sampleDigests") != [sample["sampleBlake3"] for sample in dataset["samples"]
                                              if sample["split"] == "Holdout"]:
        raise SystemExit("Independent single Holdout validation failed")
    return dataset_bytes, selection_bytes, holdout_bytes, dataset, selection, holdout


def BuildFiles(report_root: Path) -> dict[str, bytes]:
    dataset_bytes, selection_bytes, holdout_bytes, dataset, _, _ = LoadReports(report_root)
    codebook = CodebookBinary()
    mapping = MappingBinary()
    chroma_states = ChromaStatesBinary()
    stream_digest = MappingStreamDigest()
    files = {
        "dataset-manifest.json": dataset_bytes,
        "selection-report.json": selection_bytes,
        "holdout-report.json": holdout_bytes,
        "codebook.bin": codebook,
        "mapping-contract.bin": mapping,
        "chroma-states.bin": chroma_states,
        "mapping-stream.blake3": (stream_digest + "\n").encode("ascii"),
    }
    inventory = [{"file": name, "bytes": len(content), "blake3": ArtifactDigest(content)}
                 for name, content in sorted(files.items())]
    manifest = {
        "schema": "PixelBridge.UnifiedLc4MappingGolden.1",
        "status": "FrozenValidationWinnerPassedSingleHoldout",
        "profile": {"name": "PB-Unified-LC4-V1", "profileIdHex": "5042554e494c4331", "layoutVersion": 8},
        "datasetManifestBlake3": dataset["manifestBlake3"],
        "selectionReportBlake3": Blake3(selection_bytes),
        "holdoutReportBlake3": Blake3(holdout_bytes),
        "maskLabelDigest": ArtifactDigest(codebook),
        "chromaLabelDigest": ArtifactDigest(chroma_states),
        "laneInterleaveDigest": ArtifactDigest(mapping),
        "mappingStreamFrameSequenceZeroBlake3": stream_digest,
        "mappingStreamFormat": "BLAKE3(domain || repeated LE lane/carrier/plane/reserved/logical/tile records)",
        "independentRebuild": "tests/PBModulation/generate_unified_lc4_mapping_golden.py --check",
        "files": inventory,
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
        RequireRegularFile(path)
        if path.read_bytes() != content:
            raise SystemExit(f"Golden mismatch, never re-pin: {path}")


def WriteFiles(root: Path, files: dict[str, bytes]) -> None:
    if root.exists():
        raise SystemExit(f"Refusing existing output directory: {root}")
    root.mkdir(parents=True)
    for name, content in sorted(files.items()):
        with (root / name).open("xb") as output:
            output.write(content)


def Main() -> None:
    parser = argparse.ArgumentParser(description="Independently rebuild the frozen Unified LC4 mapping Golden")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--golden-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--report-root", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.check:
        if args.output_dir is not None or args.report_root is not None:
            parser.error("--check is read-only and cannot be combined with --output-dir/--report-root")
        files = BuildFiles(args.golden_root)
        CheckFiles(args.golden_root, files)
    else:
        if args.output_dir is None:
            parser.error("generation requires --output-dir")
        report_root = args.report_root if args.report_root is not None else args.golden_root
        files = BuildFiles(report_root)
        WriteFiles(args.output_dir, files)
    print(f"UNIFIED_LC4_INDEPENDENT_GOLDEN_PASS files={len(files)} "
          f"mapping_stream_blake3={MappingStreamDigest()}")


if __name__ == "__main__":
    Main()
