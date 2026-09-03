from __future__ import annotations

import argparse
from dataclasses import dataclass
import itertools
import json
import math
from pathlib import Path
import struct
import sys
from typing import Any, Iterable

from blake3 import blake3
import numpy as np

import dataset_manifest


sys.dont_write_bytecode = True

PROGRAM_VERSION = "PBUnifiedMappingSearch.1"
SEARCH_SEED_HEX = "51a7c4e29bd6038f1470ee39aa62d5c1"
SELECTION_SCHEMA = "PixelBridge.UnifiedLc4SelectionReport.1"
HOLDOUT_SCHEMA = "PixelBridge.UnifiedLc4HoldoutReport.1"
GOLDEN_SCHEMA = "PixelBridge.UnifiedLc4MappingGolden.1"
SOURCE_LOCK_SCHEMA = "PixelBridge.UnifiedLc4SearchSourceLock.1"
MAPPING_STREAM_DOMAIN = b"PixelBridge.UnifiedLc4MappingStream.1\0"
ARTIFACT_DOMAIN = b"PixelBridge.UnifiedLc4MappingArtifact.1\0"
DATA_TILE_COUNT = 86688
INNER_CODEWORD_BITS = 16200
BASE_BITS = 275400
FINE_BITS = 64800
CHROMA_BITS = 162000
BASE_DEDICATED_BITS = DATA_TILE_COUNT * 3
BASE_SHARED_BITS = BASE_BITS - BASE_DEDICATED_BITS
FINE_PLANE3_FIRST = BASE_SHARED_BITS
CHROMA_USED_TILES = CHROMA_BITS // 2
PHASE_COUNT = 16
MAPPING_CANDIDATES = 12
QUALITY_PHASES = (0, 7, 15)

DATA_REGIONS = (
    (96, 96, 1728, 64),
    (224, 160, 672, 128),
    (1024, 160, 672, 128),
    (96, 288, 1728, 188),
    (224, 476, 672, 128),
    (1024, 476, 672, 128),
    (96, 604, 1728, 188),
    (224, 792, 672, 128),
    (1024, 792, 672, 128),
    (96, 920, 1728, 64))


@dataclass(frozen=True)
class LaneInterleave:
    lane_id: int
    logical_bits: int
    multiplier: int
    inverse: int
    offset: int
    phase_step: int
    phase_count: int
    sequence_offset: int


@dataclass(frozen=True)
class MappingCandidate:
    candidate_index: int
    plane3_multiplier: int
    plane3_inverse: int
    plane3_offset: int
    chroma_tile_multiplier: int
    chroma_tile_inverse: int
    chroma_tile_offset: int
    lanes: tuple[LaneInterleave, LaneInterleave, LaneInterleave]


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


def RequireRegularFile(path: Path) -> None:
    if not path.is_file() or path.is_symlink():
        raise SystemExit(f"Required regular file missing or unsafe: {path}")


def WriteNewFile(path: Path, content: bytes) -> None:
    if path.exists():
        raise SystemExit(f"Refusing existing output file: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(content)


def WriteNewDirectory(path: Path, files: dict[str, bytes]) -> None:
    if path.exists():
        raise SystemExit(f"Refusing existing output directory: {path}")
    path.mkdir(parents=True)
    for name, content in sorted(files.items()):
        if Path(name).name != name:
            raise SystemExit(f"Unsafe output artifact name: {name}")
        with (path / name).open("xb") as output:
            output.write(content)


def LoadDataset(path: Path) -> dict[str, Any]:
    RequireRegularFile(path)
    manifest = json.loads(path.read_text(encoding="utf-8"))
    dataset_manifest.ValidateManifest(manifest)
    return manifest


def RepositoryRoot() -> Path:
    return Path(__file__).resolve().parents[2]


def SourceLock(repository_root: Path) -> dict[str, Any]:
    relative_paths = (
        "tools/PBUnifiedMappingSearch/dataset_manifest.py",
        "tools/PBUnifiedMappingSearch/unified_mapping_search.py",
        "libs/PBRemoteVisualSimulator/include/pbremotevisualsimulator/channel_transform.h",
        "libs/PBRemoteVisualSimulator/src/channel_transform.cpp")
    files = []
    for relative in relative_paths:
        path = repository_root / relative
        RequireRegularFile(path)
        content = path.read_bytes()
        files.append({"path": relative, "bytes": len(content), "blake3": Blake3(content)})
    core = {
        "schema": SOURCE_LOCK_SCHEMA,
        "programVersion": PROGRAM_VERSION,
        "searchSeedHex": SEARCH_SEED_HEX,
        "files": files,
    }
    return {**core, "sourceLockBlake3": Blake3(CanonicalJson(core))}


def VerifySourceLock(expected: dict[str, Any], repository_root: Path) -> None:
    actual = SourceLock(repository_root)
    if actual != expected:
        raise SystemExit("Search source lock mismatch; Holdout cannot be opened after source drift")


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


def CandidateMasks(manifest: dict[str, Any]) -> tuple[np.ndarray, np.ndarray]:
    historical = {int(mask_hex, 16) for source in manifest["historicalSources"]
                  for mask_hex in source["masksHexByHistoricalLabel"]}
    structural_masks = {mask for mask in range(1 << 16) if mask.bit_count() == 8 and HasNoIsolatedCells(mask)}
    masks = set(structural_masks)
    masks.update(historical)
    masks.update(mask ^ 0xFFFF for mask in historical)
    ordered = np.array(sorted(masks), dtype=np.uint16)
    representatives = np.array(sorted({min(mask, mask ^ 0xFFFF) for mask in structural_masks}), dtype=np.uint16)
    if any(int(mask).bit_count() != 8 for mask in ordered):
        raise RuntimeError("candidate enumeration lost balance")
    return ordered, representatives


def Q12Multiply(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    return ((left.astype(np.int64) @ right.astype(np.int64) + 2048) // 4096).astype(np.int64)


def GaussianOperator() -> np.ndarray:
    output = np.zeros((16, 16), dtype=np.int64)
    weights = (1, 2, 1)
    for output_y in range(4):
        for output_x in range(4):
            row = output_y * 4 + output_x
            for offset_y, weight_y in zip((-1, 0, 1), weights):
                source_y = min(3, max(0, output_y + offset_y))
                for offset_x, weight_x in zip((-1, 0, 1), weights):
                    source_x = min(3, max(0, output_x + offset_x))
                    output[row, source_y * 4 + source_x] += weight_y * weight_x * 256
    return output


def ShiftOperator(origin_q8: list[int]) -> np.ndarray:
    output = np.zeros((16, 16), dtype=np.int64)
    for output_y in range(4):
        for output_x in range(4):
            source_x_q8 = output_x * 256 - int(origin_q8[0])
            source_y_q8 = output_y * 256 - int(origin_q8[1])
            source_x0 = source_x_q8 // 256
            source_y0 = source_y_q8 // 256
            fraction_x = source_x_q8 - source_x0 * 256
            fraction_y = source_y_q8 - source_y0 * 256
            row = output_y * 4 + output_x
            for local_y, weight_y in ((0, 256 - fraction_y), (1, fraction_y)):
                source_y = min(3, max(0, source_y0 + local_y))
                for local_x, weight_x in ((0, 256 - fraction_x), (1, fraction_x)):
                    source_x = min(3, max(0, source_x0 + local_x))
                    output[row, source_y * 4 + source_x] += (weight_x * weight_y + 8) // 16
            row_sum = int(output[row].sum())
            output[row, min(15, max(0, source_y0) * 4 + min(3, max(0, source_x0)))] += 4096 - row_sum
    return output


def TileOperator(sample: dict[str, Any]) -> np.ndarray:
    transforms = sample["transforms"]
    scale = sample["scale"]
    identity = np.eye(16, dtype=np.int64) * 4096
    gaussian = GaussianOperator()
    numerator = int(scale["numerator"])
    denominator = int(scale["denominator"])
    downscale_q12 = max(0, denominator - numerator) * 4096 // denominator
    filter_base = {
        "area": 0,
        "bilinear": 384,
        "bicubic-catmull-rom-q16": 128,
    }[transforms["resampleFilter"]]
    blend_q12 = min(3072, filter_base + downscale_q12 * 3 // 4)
    operator = ((identity * (4096 - blend_q12) + gaussian * blend_q12 + 2048) // 4096).astype(np.int64)
    operator = Q12Multiply(ShiftOperator(transforms["originQ8"]), operator)
    for _ in range(int(transforms["gaussianBlurPasses"])):
        operator = Q12Multiply(gaussian, operator)
    return operator


def Quantize(values: np.ndarray, bits_per_channel: int) -> np.ndarray:
    maximum_level = (1 << bits_per_channel) - 1
    levels = (values.astype(np.int64) * maximum_level + 127) // 255
    return ((levels * 255 + maximum_level // 2) // maximum_level).astype(np.int64)


def RoundSignedDivision(numerator: int, denominator: int) -> int:
    return (numerator + denominator // 2) // denominator if numerator >= 0 else \
        -((-numerator + denominator // 2) // denominator)


def MaskPatterns(masks: np.ndarray, low_luma: int, high_luma: int) -> np.ndarray:
    shifts = np.arange(16, dtype=np.uint16)
    bits = ((masks[:, None] >> shifts[None, :]) & 1).astype(np.int64)
    return low_luma + bits * (high_luma - low_luma)


def CenterPatterns(patterns: np.ndarray) -> np.ndarray:
    return patterns.astype(np.int64) * 16 - patterns.astype(np.int64).sum(axis=1, keepdims=True)


def SourceById(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {source["id"]: source for source in manifest["historicalSources"]}


def Observations(manifest: dict[str, Any], masks: np.ndarray, split: str) -> tuple[list[dict[str, Any]], np.ndarray]:
    sources = SourceById(manifest)
    samples = [sample for sample in manifest["samples"] if sample["split"] == split]
    observed = []
    for sample in samples:
        source = sources[sample["sourceId"]]
        patterns = MaskPatterns(masks, int(source["lowLuma"]), int(source["highLuma"]))
        transformed = (patterns @ TileOperator(sample).T + 2048) // 4096
        transformed = Quantize(transformed, int(sample["transforms"]["quantizationBitsPerChannel"]))
        observed.append(CenterPatterns(transformed))
    return samples, np.stack(observed, axis=0)


def MinimumDistancesToSelection(observed: np.ndarray, candidate_indices: np.ndarray,
                                complement_indices: np.ndarray, selected_indices: list[int]) -> np.ndarray:
    minimum = np.full(candidate_indices.shape[0], np.iinfo(np.int64).max, dtype=np.int64)
    for selected_index in selected_indices:
        selected = observed[:, selected_index:selected_index + 1, :]
        direct = np.abs(observed[:, candidate_indices, :] - selected).sum(axis=2).min(axis=0)
        complement = np.abs(observed[:, complement_indices, :] - selected).sum(axis=2).min(axis=0)
        minimum = np.minimum(minimum, np.minimum(direct, complement))
    return minimum


def GreedyPairSet(anchor: int, masks: np.ndarray, representatives: np.ndarray,
                  observed: np.ndarray) -> tuple[int, ...]:
    index_by_mask = {int(mask): index for index, mask in enumerate(masks)}
    representative_indices = np.array([index_by_mask[int(mask)] for mask in representatives], dtype=np.int64)
    complement_indices = np.array([index_by_mask[int(mask) ^ 0xFFFF] for mask in representatives], dtype=np.int64)
    selected = [anchor]
    while len(selected) < 8:
        selected_mask_indices = [index_by_mask[mask] for representative in selected
                                 for mask in (representative, representative ^ 0xFFFF)]
        distances = MinimumDistancesToSelection(observed, representative_indices, complement_indices,
                                                selected_mask_indices)
        selected_masks = [mask for representative in selected for mask in (representative, representative ^ 0xFFFF)]
        for candidate_index, representative in enumerate(representatives):
            candidate = int(representative)
            candidate_masks = (candidate, candidate ^ 0xFFFF)
            if candidate in selected or min((candidate_mask ^ selected_mask).bit_count()
                                             for candidate_mask in candidate_masks
                                             for selected_mask in selected_masks) < 8:
                distances[candidate_index] = -1
        best_distance = int(distances.max())
        if best_distance < 0:
            raise RuntimeError(f"no Hamming-compatible candidate remains for anchor {anchor:04x}")
        best_candidates = representatives[distances == best_distance]
        selected.append(int(best_candidates.min()))
    return tuple(sorted(selected))


def CodebookPairSets(manifest: dict[str, Any], masks: np.ndarray, representatives: np.ndarray,
                     train_observed: np.ndarray) -> list[tuple[int, ...]]:
    historical_pair_sets = []
    for source in manifest["historicalSources"]:
        historical_pair_sets.append(tuple(sorted({min(int(mask_hex, 16), int(mask_hex, 16) ^ 0xFFFF)
                                                  for mask_hex in source["masksHexByHistoricalLabel"]})))
    eligible_representatives = {int(value) for value in representatives}
    anchors = sorted({representative for pair_set in historical_pair_sets for representative in pair_set
                      if representative in eligible_representatives})
    anchors.extend(int(value) for value in representatives[:4] if int(value) not in anchors)
    candidates = set()
    for pair_set in historical_pair_sets:
        codebook = list(pair_set) + [mask ^ 0xFFFF for mask in pair_set]
        if len(pair_set) == 8 and all(representative in eligible_representatives for representative in pair_set) and \
                min((left ^ right).bit_count() for index, left in enumerate(codebook)
                    for right in codebook[index + 1:]) >= 8:
            candidates.add(pair_set)
    for anchor in anchors:
        try:
            candidates.add(GreedyPairSet(anchor, masks, representatives, train_observed))
        except RuntimeError:
            continue
    if not candidates:
        raise RuntimeError("no structurally valid codebook candidate was generated")
    return sorted(candidates)


def MinimumDistanceMatrix(observed: np.ndarray, indices: list[int]) -> np.ndarray:
    selected = observed[:, indices, :]
    difference = np.abs(selected[:, :, None, :] - selected[:, None, :, :]).sum(axis=3)
    return difference.min(axis=0).astype(np.int64)


def OptimizeLabels(pair_set: tuple[int, ...], masks: np.ndarray, train_observed: np.ndarray) -> tuple[int, ...]:
    index_by_mask = {int(mask): index for index, mask in enumerate(masks)}
    unordered_masks = list(pair_set) + [mask ^ 0xFFFF for mask in pair_set]
    distance = MinimumDistanceMatrix(train_observed, [index_by_mask[mask] for mask in unordered_masks])
    group_distance = np.zeros((8, 8), dtype=np.int64)
    for left in range(8):
        for right in range(8):
            group_distance[left, right] = min(
                int(distance[left, right]), int(distance[left, right + 8]),
                int(distance[left + 8, right]), int(distance[left + 8, right + 8]))
    partition_scores: dict[int, int] = {}
    for subset in range(1 << 8):
        if subset.bit_count() != 4:
            continue
        cross = [int(group_distance[left, right]) for left in range(8) for right in range(left + 1, 8)
                 if ((subset >> left) & 1) != ((subset >> right) & 1)]
        partition_scores[subset] = min(cross)
    best_key: tuple[Any, ...] | None = None
    best_masks: tuple[int, ...] | None = None
    for permutation in itertools.permutations(range(8)):
        bit_scores = []
        for bit in range(3):
            subset = sum(1 << permutation[label] for label in range(8) if (label >> bit) & 1)
            bit_scores.append(partition_scores[subset])
        ordered = tuple(pair_set[index] for index in permutation)
        masks_by_label = ordered + tuple(mask ^ 0xFFFF for mask in ordered)
        key = (min(bit_scores), tuple(sorted(bit_scores)), tuple(-mask for mask in masks_by_label))
        if best_key is None or key > best_key:
            best_key = key
            best_masks = masks_by_label
    if best_masks is None:
        raise RuntimeError("label search produced no result")
    return best_masks


def SignedBitMargins(manifest: dict[str, Any], masks_by_label: tuple[int, ...], split: str,
                     all_masks: np.ndarray, observed: np.ndarray) -> dict[str, Any]:
    sources = SourceById(manifest)
    samples = [sample for sample in manifest["samples"] if sample["split"] == split]
    index_by_mask = {int(mask): index for index, mask in enumerate(all_masks)}
    selected_indices = [index_by_mask[mask] for mask in masks_by_label]
    selected_observed = observed[:, selected_indices, :]
    signed_by_bit: list[list[int]] = [[] for _ in range(4)]
    false_by_bit = [0, 0, 0, 0]
    erased_by_bit = [0, 0, 0, 0]
    accepted_by_bit = [0, 0, 0, 0]
    for sample_index, sample in enumerate(samples):
        source = sources[sample["sourceId"]]
        ideal = CenterPatterns(MaskPatterns(np.array(masks_by_label, dtype=np.uint16),
                                           int(source["lowLuma"]), int(source["highLuma"])))
        distances = np.abs(selected_observed[sample_index, :, None, :] - ideal[None, :, :]).sum(axis=2)
        for true_label in range(16):
            for bit in range(4):
                zero_labels = [label for label in range(16) if ((label >> bit) & 1) == 0]
                one_labels = [label for label in range(16) if ((label >> bit) & 1) == 1]
                zero_distance = int(distances[true_label, zero_labels].min())
                one_distance = int(distances[true_label, one_labels].min())
                predicted = 1 if one_distance < zero_distance else 0
                margin = abs(one_distance - zero_distance)
                correct = predicted == ((true_label >> bit) & 1)
                signed = margin if correct else -margin
                signed_by_bit[bit].append(signed)
                if margin == 0:
                    erased_by_bit[bit] += 1
                elif correct:
                    accepted_by_bit[bit] += 1
                else:
                    false_by_bit[bit] += 1
    minima = [min(values) for values in signed_by_bit]
    return {
        "minimumSignedMarginByLabelBit": minima,
        "baseMinimumSignedMargin": min(minima),
        "fineMinimumSignedMargin": minima[3],
        "acceptedByLabelBit": accepted_by_bit,
        "erasedByLabelBit": erased_by_bit,
        "falseAcceptedByLabelBit": false_by_bit,
        "falseAcceptedBase": sum(false_by_bit),
        "falseAcceptedFine": false_by_bit[3],
        "observationsPerLabelBit": len(samples) * 16,
    }


def ChromaStats(manifest: dict[str, Any], split: str) -> dict[str, Any]:
    sources = SourceById(manifest)
    samples = [sample for sample in manifest["samples"] if sample["split"] == split]
    states_by_label = sorted(dataset_manifest.CHROMA_STATES, key=lambda state: state[3])
    margins: list[int] = []
    false_accepted = 0
    accepted = 0
    erased = 0
    unavailable = 0
    for sample in samples:
        if sample["transforms"]["neutralChroma"]:
            unavailable += 8
            continue
        scale = sample["scale"]
        scale_q12 = min(4096, int(scale["numerator"]) * 4096 // int(scale["denominator"]))
        if sample["transforms"]["chroma420"]:
            scale_q12 = scale_q12 * 3 // 4
        observed_states = []
        ideal_states = []
        for blue, green, red, _ in states_by_label:
            ideal_states.append((128 + blue, 128 + green, 128 + red))
            values = np.array([[128 + RoundSignedDivision(blue * scale_q12, 4096),
                                128 + RoundSignedDivision(green * scale_q12, 4096),
                                128 + RoundSignedDivision(red * scale_q12, 4096)]], dtype=np.int64)
            quantized = Quantize(values, int(sample["transforms"]["quantizationBitsPerChannel"]))[0]
            observed_states.append(tuple(int(value) for value in quantized))
        distances = np.abs(np.array(observed_states)[:, None, :] - np.array(ideal_states)[None, :, :]).sum(axis=2)
        for true_label in range(4):
            for bit in range(2):
                zero_labels = [label for label in range(4) if ((label >> bit) & 1) == 0]
                one_labels = [label for label in range(4) if ((label >> bit) & 1) == 1]
                zero_distance = int(distances[true_label, zero_labels].min())
                one_distance = int(distances[true_label, one_labels].min())
                predicted = 1 if one_distance < zero_distance else 0
                margin = abs(one_distance - zero_distance)
                correct = predicted == ((true_label >> bit) & 1)
                margins.append(margin if correct else -margin)
                if margin == 0:
                    erased += 1
                elif correct:
                    accepted += 1
                else:
                    false_accepted += 1
    return {
        "minimumSignedMargin": min(margins) if margins else 0,
        "accepted": accepted,
        "erased": erased,
        "falseAccepted": false_accepted,
        "expectedUnavailable": unavailable,
        "evaluated": len(margins),
    }


def CoprimeAtOrAfter(value: int, modulus: int) -> int:
    candidate = value % modulus
    if candidate < 2:
        candidate = 2
    while math.gcd(candidate, modulus) != 1:
        candidate += 1
        if candidate == modulus:
            candidate = 2
    return candidate


def DeriveNumber(candidate_index: int, name: str) -> int:
    content = bytes.fromhex(SEARCH_SEED_HEX) + struct.pack("<I", candidate_index) + name.encode("ascii")
    return int.from_bytes(blake3(content).digest(length=8), "little")


def DeriveMappingCandidate(candidate_index: int) -> MappingCandidate:
    plane3_multiplier = CoprimeAtOrAfter(DeriveNumber(candidate_index, "plane3-multiplier"), DATA_TILE_COUNT)
    chroma_multiplier = CoprimeAtOrAfter(DeriveNumber(candidate_index, "chroma-tile-multiplier"), DATA_TILE_COUNT)
    lane_sizes = (BASE_BITS, FINE_BITS, CHROMA_BITS)
    lanes = []
    for lane_id, logical_bits in enumerate(lane_sizes):
        prefix = f"lane-{lane_id}"
        multiplier = CoprimeAtOrAfter(DeriveNumber(candidate_index, prefix + "-multiplier"), logical_bits)
        phase_step = CoprimeAtOrAfter(DeriveNumber(candidate_index, prefix + "-phase-step"), logical_bits)
        lanes.append(LaneInterleave(
            lane_id=lane_id,
            logical_bits=logical_bits,
            multiplier=multiplier,
            inverse=pow(multiplier, -1, logical_bits),
            offset=DeriveNumber(candidate_index, prefix + "-offset") % logical_bits,
            phase_step=phase_step,
            phase_count=PHASE_COUNT,
            sequence_offset=DeriveNumber(candidate_index, prefix + "-sequence-offset") % PHASE_COUNT))
    return MappingCandidate(
        candidate_index=candidate_index,
        plane3_multiplier=plane3_multiplier,
        plane3_inverse=pow(plane3_multiplier, -1, DATA_TILE_COUNT),
        plane3_offset=DeriveNumber(candidate_index, "plane3-offset") % DATA_TILE_COUNT,
        chroma_tile_multiplier=chroma_multiplier,
        chroma_tile_inverse=pow(chroma_multiplier, -1, DATA_TILE_COUNT),
        chroma_tile_offset=DeriveNumber(candidate_index, "chroma-tile-offset") % DATA_TILE_COUNT,
        lanes=tuple(lanes))


def MappingToJson(mapping: MappingCandidate) -> dict[str, Any]:
    return {
        "candidateIndex": mapping.candidate_index,
        "plane3TileOrder": {
            "multiplier": mapping.plane3_multiplier,
            "inverse": mapping.plane3_inverse,
            "offset": mapping.plane3_offset,
            "modulus": DATA_TILE_COUNT,
        },
        "chromaTileOrder": {
            "multiplier": mapping.chroma_tile_multiplier,
            "inverse": mapping.chroma_tile_inverse,
            "offset": mapping.chroma_tile_offset,
            "modulus": DATA_TILE_COUNT,
            "usedTiles": CHROMA_USED_TILES,
        },
        "lanes": [
            {
                "laneId": lane.lane_id,
                "logicalBits": lane.logical_bits,
                "multiplier": lane.multiplier,
                "inverse": lane.inverse,
                "offset": lane.offset,
                "phaseStep": lane.phase_step,
                "phaseCount": lane.phase_count,
                "sequenceOffset": lane.sequence_offset,
            }
            for lane in mapping.lanes],
        "lumaOwnership": {
            "baseDedicatedPlanes": [0, 1, 2],
            "baseDedicatedBits": BASE_DEDICATED_BITS,
            "basePlane3Positions": [0, BASE_SHARED_BITS],
            "finePlane3Positions": [FINE_PLANE3_FIRST, FINE_PLANE3_FIRST + FINE_BITS],
            "reservedPlane3Positions": [FINE_PLANE3_FIRST + FINE_BITS, DATA_TILE_COUNT],
        },
        "phaseEvaluation": list(QUALITY_PHASES),
    }


def TileCoordinates() -> tuple[np.ndarray, np.ndarray]:
    x_values = []
    y_values = []
    for x, y, width, height in DATA_REGIONS:
        if x % 4 or y % 4 or width % 4 or height % 4:
            raise RuntimeError("data region is not tile aligned")
        for tile_y in range(y // 4, (y + height) // 4):
            for tile_x in range(x // 4, (x + width) // 4):
                x_values.append(tile_x)
                y_values.append(tile_y)
    if len(x_values) != DATA_TILE_COUNT:
        raise RuntimeError(f"unexpected data tile count: {len(x_values)}")
    return np.array(x_values, dtype=np.int64), np.array(y_values, dtype=np.int64)


def TileQualityQ12(sample: dict[str, Any], lane_id: int, tile_x: np.ndarray, tile_y: np.ndarray) -> np.ndarray:
    transforms = sample["transforms"]
    scale = sample["scale"]
    numerator = int(scale["numerator"])
    denominator = int(scale["denominator"])
    origin_x, origin_y = (int(value) for value in transforms["originQ8"])
    phase_x = (tile_x * 4 * numerator * 256 // denominator + origin_x) % 256
    phase_y = (tile_y * 4 * numerator * 256 // denominator + origin_y) % 256
    phase_distance = np.minimum(phase_x, 256 - phase_x) + np.minimum(phase_y, 256 - phase_y)
    quality = np.full(tile_x.shape, 4096, dtype=np.int64) - phase_distance * 2
    quality -= max(0, denominator - numerator) * 640 // denominator
    quality -= int(transforms["gaussianBlurPasses"]) * 224
    quality -= (8 - int(transforms["quantizationBitsPerChannel"])) * 64
    if lane_id == 2 and transforms["chroma420"]:
        quality -= 640
    sample_phase = int(sample["sampleBlake3"][:8], 16)
    stripes = ((tile_x + sample_phase) % 17 == 0) | ((tile_y + (sample_phase >> 8)) % 13 == 0)
    quality[stripes] -= 288
    minimum = int(sample["spatialQualityModel"]["minimumQualityQ12"])
    return np.clip(quality, minimum, 4096)


def MapLogicalBits(mapping: MappingCandidate, lane_id: int, frame_sequence: int) -> tuple[np.ndarray, np.ndarray]:
    lane = mapping.lanes[lane_id]
    logical = np.arange(lane.logical_bits, dtype=np.int64)
    phase = (frame_sequence + lane.sequence_offset) % lane.phase_count
    domain = (logical * lane.multiplier + lane.offset + phase * lane.phase_step) % lane.logical_bits
    if lane_id == 0:
        dedicated = domain < BASE_DEDICATED_BITS
        tile = np.empty(domain.shape, dtype=np.int64)
        plane = np.empty(domain.shape, dtype=np.int64)
        tile[dedicated] = domain[dedicated] // 3
        plane[dedicated] = domain[dedicated] % 3
        plane3_position = domain[~dedicated] - BASE_DEDICATED_BITS
        tile[~dedicated] = (plane3_position * mapping.plane3_multiplier + mapping.plane3_offset) % DATA_TILE_COUNT
        plane[~dedicated] = 3
        return tile, plane
    if lane_id == 1:
        plane3_position = domain + FINE_PLANE3_FIRST
        tile = (plane3_position * mapping.plane3_multiplier + mapping.plane3_offset) % DATA_TILE_COUNT
        return tile, np.full(domain.shape, 3, dtype=np.int64)
    used_tile_position = domain // 2
    tile = (used_tile_position * mapping.chroma_tile_multiplier + mapping.chroma_tile_offset) % DATA_TILE_COUNT
    return tile, domain % 2


def MappingCoverage(manifest: dict[str, Any], mapping: MappingCandidate, split: str) -> list[int]:
    samples = [sample for sample in manifest["samples"] if sample["split"] == split]
    tile_x, tile_y = TileCoordinates()
    lane_minimum = [4096, 4096, 4096]
    for lane_id in range(3):
        lane = mapping.lanes[lane_id]
        if lane.logical_bits % INNER_CODEWORD_BITS:
            raise RuntimeError("lane is not an integral codeword set")
        mapped_by_phase = [MapLogicalBits(mapping, lane_id, phase)[0] for phase in QUALITY_PHASES]
        for sample in samples:
            if lane_id == 2 and sample["transforms"]["neutralChroma"]:
                continue
            quality = TileQualityQ12(sample, lane_id, tile_x, tile_y)
            for tile in mapped_by_phase:
                codeword_quality = quality[tile].reshape((-1, INNER_CODEWORD_BITS)).sum(axis=1) // INNER_CODEWORD_BITS
                lane_minimum[lane_id] = min(lane_minimum[lane_id], int(codeword_quality.min()))
    return lane_minimum


def CodebookBinary(masks_by_label: tuple[int, ...]) -> bytes:
    return struct.pack("<16H", *masks_by_label)


def ChromaStatesBinary() -> bytes:
    states_by_label = sorted(dataset_manifest.CHROMA_STATES, key=lambda state: state[3])
    return b"".join(struct.pack("<hhhB", *state) for state in states_by_label)


def MappingBinary(mapping: MappingCandidate) -> bytes:
    output = bytearray(b"PBULC4M1")
    output += struct.pack("<IIIIII", 1, DATA_TILE_COUNT, mapping.plane3_multiplier, mapping.plane3_offset,
                          mapping.chroma_tile_multiplier, mapping.chroma_tile_offset)
    for lane in mapping.lanes:
        output += struct.pack("<IIIIIIII", lane.lane_id, lane.logical_bits, lane.multiplier, lane.inverse,
                              lane.offset, lane.phase_step, lane.phase_count, lane.sequence_offset)
    return bytes(output)


def MappingStreamDigest(mapping: MappingCandidate) -> str:
    hasher = blake3()
    hasher.update(MAPPING_STREAM_DOMAIN)
    for lane_id in range(3):
        tiles, planes = MapLogicalBits(mapping, lane_id, 0)
        carrier = 0 if lane_id < 2 else 1
        for logical, (tile, plane) in enumerate(zip(tiles, planes)):
            hasher.update(struct.pack("<BBBBII", lane_id, carrier, int(plane), 0, logical, int(tile)))
    return hasher.hexdigest()


def MappingFromJson(value: dict[str, Any]) -> MappingCandidate:
    lanes = tuple(LaneInterleave(
        lane_id=int(lane["laneId"]), logical_bits=int(lane["logicalBits"]), multiplier=int(lane["multiplier"]),
        inverse=int(lane["inverse"]), offset=int(lane["offset"]), phase_step=int(lane["phaseStep"]),
        phase_count=int(lane["phaseCount"]), sequence_offset=int(lane["sequenceOffset"]))
        for lane in value["lanes"])
    return MappingCandidate(
        candidate_index=int(value["candidateIndex"]),
        plane3_multiplier=int(value["plane3TileOrder"]["multiplier"]),
        plane3_inverse=int(value["plane3TileOrder"]["inverse"]),
        plane3_offset=int(value["plane3TileOrder"]["offset"]),
        chroma_tile_multiplier=int(value["chromaTileOrder"]["multiplier"]),
        chroma_tile_inverse=int(value["chromaTileOrder"]["inverse"]),
        chroma_tile_offset=int(value["chromaTileOrder"]["offset"]),
        lanes=lanes)


def EvaluateCandidate(manifest: dict[str, Any], split: str, masks_by_label: tuple[int, ...], mapping: MappingCandidate,
                      all_masks: np.ndarray, observed: np.ndarray, chroma_stats: dict[str, Any]) -> dict[str, Any]:
    luma = SignedBitMargins(manifest, masks_by_label, split, all_masks, observed)
    coverage = MappingCoverage(manifest, mapping, split)
    scores = {
        "baseMinimumMarginQ12": luma["baseMinimumSignedMargin"] * coverage[0],
        "fineMinimumMarginQ12": luma["fineMinimumSignedMargin"] * coverage[1],
        "chromaMinimumMarginQ12": chroma_stats["minimumSignedMargin"] * coverage[2],
    }
    return {
        "scores": scores,
        "signal": {"luma": luma, "chroma": chroma_stats},
        "minimumCodewordQualityQ12": {"Base": coverage[0], "Fine": coverage[1], "Chroma": coverage[2]},
        "falseAccepted": {
            "Base": luma["falseAcceptedBase"],
            "Fine": luma["falseAcceptedFine"],
            "Chroma": chroma_stats["falseAccepted"],
        },
    }


def RankingTuple(evaluation: dict[str, Any], masks_by_label: tuple[int, ...], mapping: MappingCandidate) -> tuple[Any, ...]:
    scores = evaluation["scores"]
    lexical = "-".join(f"{mask:04x}" for mask in masks_by_label) + ":" + CanonicalJson(MappingToJson(mapping)).decode("utf-8")
    return (-int(scores["baseMinimumMarginQ12"]), -int(scores["fineMinimumMarginQ12"]),
            -int(scores["chromaMinimumMarginQ12"]), lexical)


def Select(manifest: dict[str, Any], dataset_path: Path, output_directory: Path) -> None:
    if output_directory.exists():
        raise SystemExit(f"Refusing existing output directory: {output_directory}")
    repository_root = RepositoryRoot()
    source_lock = SourceLock(repository_root)
    all_masks, representatives = CandidateMasks(manifest)
    train_samples, train_observed = Observations(manifest, all_masks, "Train")
    validation_samples, validation_observed = Observations(manifest, all_masks, "Validation")
    pair_sets = CodebookPairSets(manifest, all_masks, representatives, train_observed)
    labeled_codebooks = []
    for pair_set in pair_sets:
        masks_by_label = OptimizeLabels(pair_set, all_masks, train_observed)
        if masks_by_label not in labeled_codebooks:
            labeled_codebooks.append(masks_by_label)
    mappings = [DeriveMappingCandidate(index) for index in range(MAPPING_CANDIDATES)]
    train_chroma = ChromaStats(manifest, "Train")
    validation_chroma = ChromaStats(manifest, "Validation")
    ranked = []
    for masks_by_label in labeled_codebooks:
        for mapping in mappings:
            validation = EvaluateCandidate(manifest, "Validation", masks_by_label, mapping, all_masks,
                                           validation_observed, validation_chroma)
            ranked.append((RankingTuple(validation, masks_by_label, mapping), masks_by_label, mapping, validation))
    ranked.sort(key=lambda item: item[0])
    _, winner_masks, winner_mapping, validation = ranked[0]
    train = EvaluateCandidate(manifest, "Train", winner_masks, winner_mapping, all_masks, train_observed, train_chroma)
    if any(value <= 0 for value in validation["scores"].values()) or any(validation["falseAccepted"].values()):
        raise SystemExit("Validation winner did not satisfy positive-margin/zero-false-acceptance gate")
    score_rows = [
        {
            "masksHexByLabel": [f"{mask:04x}" for mask in masks],
            "mappingCandidateIndex": mapping.candidate_index,
            "scores": evaluation["scores"],
            "minimumCodewordQualityQ12": evaluation["minimumCodewordQualityQ12"],
            "falseAccepted": evaluation["falseAccepted"],
        }
        for _, masks, mapping, evaluation in ranked]
    codebook = CodebookBinary(winner_masks)
    mapping_contract = MappingBinary(winner_mapping)
    chroma_states = ChromaStatesBinary()
    report = {
        "schema": SELECTION_SCHEMA,
        "programVersion": PROGRAM_VERSION,
        "runtime": {"python": ".".join(str(value) for value in sys.version_info[:3]), "numpy": np.__version__},
        "status": "ValidationWinnerFrozenBeforeHoldout",
        "sourceLock": source_lock,
        "datasetManifestBlake3": manifest["manifestBlake3"],
        "search": {
            "seedHex": SEARCH_SEED_HEX,
            "candidateEnumeration": "balanced 4x4 masks with no isolated equal-valued 4-neighbor cell, plus historical probes; every selectable codebook has pairwise Hamming distance at least 8",
            "candidateMasks": int(all_masks.size),
            "searchEligibleMasks": int(representatives.size * 2),
            "complementPairRepresentatives": int(representatives.size),
            "codebookPairSets": len(pair_sets),
            "labeledCodebooks": len(labeled_codebooks),
            "mappingCandidates": len(mappings),
            "validationCandidates": len(ranked),
            "labelSearch": "Train-only exhaustive 8! complement-pair assignment; canonical orientation",
            "rankingOrder": ["Base minimum margin descending", "Fine minimum margin descending",
                             "Chroma minimum margin descending", "lexicographic ascending"],
            "scoreFormula": "minimum signed decision margin * minimum per-codeword spatial quality Q12; product retains Q12",
            "allValidationScoresBlake3": Blake3(CanonicalJson(score_rows)),
        },
        "splitDiscipline": {
            "trainSamplesEvaluated": len(train_samples),
            "validationSamplesEvaluated": len(validation_samples),
            "holdoutSamplesEvaluated": 0,
            "holdoutResultsOpened": False,
        },
        "winner": {
            "masksHexByLabel": [f"{mask:04x}" for mask in winner_masks],
            "chromaStatesByLabel": [list(state) for state in sorted(dataset_manifest.CHROMA_STATES,
                                                                     key=lambda state: state[3])],
            "mapping": MappingToJson(winner_mapping),
            "train": train,
            "validation": validation,
            "mappingStreamFrameSequenceZeroBlake3": MappingStreamDigest(winner_mapping),
        },
        "topValidationCandidates": score_rows[:10],
        "artifacts": {
            "codebook.bin": {"bytes": len(codebook), "blake3": ArtifactDigest(codebook), "format": "16xLE16-by-label"},
            "mapping-contract.bin": {"bytes": len(mapping_contract), "blake3": ArtifactDigest(mapping_contract),
                                     "format": "PBULC4M1/LE/v1"},
            "chroma-states.bin": {"bytes": len(chroma_states), "blake3": ArtifactDigest(chroma_states),
                                  "format": "4x(LE16-blue,LE16-green,LE16-red,u8-label)-by-label"},
        },
    }
    files = {
        "dataset-manifest.json": dataset_path.read_bytes(),
        "selection-report.json": PrettyJson(report),
        "codebook.bin": codebook,
        "mapping-contract.bin": mapping_contract,
        "chroma-states.bin": chroma_states,
    }
    WriteNewDirectory(output_directory, files)
    print(f"UNIFIED_LC4_SELECTION_PASS candidates={len(ranked)} "
          f"winner={Blake3(codebook + mapping_contract)[:16]} "
          f"base_q12={validation['scores']['baseMinimumMarginQ12']} "
          f"fine_q12={validation['scores']['fineMinimumMarginQ12']} "
          f"chroma_q12={validation['scores']['chromaMinimumMarginQ12']} holdout_opened=false")


def LoadSelection(selection_directory: Path, manifest: dict[str, Any], repository_root: Path) -> dict[str, Any]:
    expected_names = {"dataset-manifest.json", "selection-report.json", "codebook.bin", "mapping-contract.bin",
                      "chroma-states.bin"}
    if not selection_directory.is_dir() or selection_directory.is_symlink():
        raise SystemExit(f"Selection directory missing or unsafe: {selection_directory}")
    actual_names = {path.name for path in selection_directory.iterdir()}
    if actual_names != expected_names:
        raise SystemExit(f"Selection inventory mismatch: expected={sorted(expected_names)} actual={sorted(actual_names)}")
    for name in expected_names:
        RequireRegularFile(selection_directory / name)
    if (selection_directory / "dataset-manifest.json").read_bytes() != PrettyJson(manifest):
        raise SystemExit("Selection dataset manifest differs from the sealed manifest")
    report = json.loads((selection_directory / "selection-report.json").read_text(encoding="utf-8"))
    if report.get("schema") != SELECTION_SCHEMA or report.get("status") != "ValidationWinnerFrozenBeforeHoldout":
        raise SystemExit("Selection report is not a frozen Validation winner")
    if report.get("datasetManifestBlake3") != manifest["manifestBlake3"]:
        raise SystemExit("Selection report dataset digest mismatch")
    VerifySourceLock(report["sourceLock"], repository_root)
    artifact_names = ("codebook.bin", "mapping-contract.bin", "chroma-states.bin")
    for name in artifact_names:
        content = (selection_directory / name).read_bytes()
        expected = report["artifacts"][name]
        if len(content) != expected["bytes"] or ArtifactDigest(content) != expected["blake3"]:
            raise SystemExit(f"Frozen selection artifact mismatch: {name}")
    winner_masks = tuple(int(value, 16) for value in report["winner"]["masksHexByLabel"])
    mapping = MappingFromJson(report["winner"]["mapping"])
    if CodebookBinary(winner_masks) != (selection_directory / "codebook.bin").read_bytes():
        raise SystemExit("Selection codebook bytes do not match winner")
    if MappingBinary(mapping) != (selection_directory / "mapping-contract.bin").read_bytes():
        raise SystemExit("Selection mapping bytes do not match winner")
    if MappingStreamDigest(mapping) != report["winner"]["mappingStreamFrameSequenceZeroBlake3"]:
        raise SystemExit("Selection mapping stream digest does not rebuild")
    return report


def EvaluateHoldout(manifest: dict[str, Any], selection_directory: Path, output_file: Path) -> None:
    if output_file.exists():
        raise SystemExit(f"Refusing existing output file before Holdout evaluation: {output_file}")
    repository_root = RepositoryRoot()
    selection = LoadSelection(selection_directory, manifest, repository_root)
    all_masks, _ = CandidateMasks(manifest)
    holdout_samples, holdout_observed = Observations(manifest, all_masks, "Holdout")
    winner_masks = tuple(int(value, 16) for value in selection["winner"]["masksHexByLabel"])
    mapping = MappingFromJson(selection["winner"]["mapping"])
    holdout_chroma = ChromaStats(manifest, "Holdout")
    evaluation = EvaluateCandidate(manifest, "Holdout", winner_masks, mapping, all_masks,
                                   holdout_observed, holdout_chroma)
    passed = all(value > 0 for value in evaluation["scores"].values()) and not any(evaluation["falseAccepted"].values())
    report = {
        "schema": HOLDOUT_SCHEMA,
        "programVersion": PROGRAM_VERSION,
        "status": "Passed" if passed else "Failed",
        "datasetManifestBlake3": manifest["manifestBlake3"],
        "selectionReportBlake3": Blake3((selection_directory / "selection-report.json").read_bytes()),
        "sourceLockBlake3": selection["sourceLock"]["sourceLockBlake3"],
        "holdoutPolicy": {
            "winnerOnly": True,
            "singleEvaluation": True,
            "providerMetadataUsed": False,
            "failureAction": "block-without-retuning",
        },
        "sampleCount": len(holdout_samples),
        "sampleDigests": [sample["sampleBlake3"] for sample in holdout_samples],
        "evaluation": evaluation,
        "passCriteria": "all lane minimum margins positive and falseAccepted Base/Fine/Chroma all zero",
    }
    WriteNewFile(output_file, PrettyJson(report))
    print(f"UNIFIED_LC4_HOLDOUT_{'PASS' if passed else 'FAIL'} samples={len(holdout_samples)} "
          f"base_false={evaluation['falseAccepted']['Base']} fine_false={evaluation['falseAccepted']['Fine']} "
          f"chroma_false={evaluation['falseAccepted']['Chroma']}")
    if not passed:
        raise SystemExit("Frozen winner failed Holdout; do not retune on Holdout")


def BuildGolden(manifest: dict[str, Any], selection_directory: Path, holdout_file: Path,
                output_directory: Path) -> None:
    if output_directory.exists():
        raise SystemExit(f"Refusing existing output directory: {output_directory}")
    repository_root = RepositoryRoot()
    selection = LoadSelection(selection_directory, manifest, repository_root)
    RequireRegularFile(holdout_file)
    holdout_bytes = holdout_file.read_bytes()
    holdout = json.loads(holdout_bytes.decode("utf-8"))
    if holdout.get("schema") != HOLDOUT_SCHEMA or holdout.get("status") != "Passed":
        raise SystemExit("Golden requires the single passing Holdout report")
    if holdout.get("datasetManifestBlake3") != manifest["manifestBlake3"] or \
            holdout.get("selectionReportBlake3") != Blake3((selection_directory / "selection-report.json").read_bytes()):
        raise SystemExit("Holdout report is not bound to this dataset/selection")
    mapping = MappingFromJson(selection["winner"]["mapping"])
    stream_digest = MappingStreamDigest(mapping)
    files = {
        "dataset-manifest.json": (selection_directory / "dataset-manifest.json").read_bytes(),
        "selection-report.json": (selection_directory / "selection-report.json").read_bytes(),
        "holdout-report.json": holdout_bytes,
        "codebook.bin": (selection_directory / "codebook.bin").read_bytes(),
        "mapping-contract.bin": (selection_directory / "mapping-contract.bin").read_bytes(),
        "chroma-states.bin": (selection_directory / "chroma-states.bin").read_bytes(),
        "mapping-stream.blake3": (stream_digest + "\n").encode("ascii"),
    }
    inventory = [{"file": name, "bytes": len(content), "blake3": ArtifactDigest(content)}
                 for name, content in sorted(files.items())]
    golden_manifest = {
        "schema": GOLDEN_SCHEMA,
        "status": "FrozenValidationWinnerPassedSingleHoldout",
        "profile": {"name": "PB-Unified-LC4-V1", "profileIdHex": "5042554e494c4331", "layoutVersion": 8},
        "datasetManifestBlake3": manifest["manifestBlake3"],
        "selectionReportBlake3": Blake3(files["selection-report.json"]),
        "holdoutReportBlake3": Blake3(holdout_bytes),
        "maskLabelDigest": ArtifactDigest(files["codebook.bin"]),
        "chromaLabelDigest": ArtifactDigest(files["chroma-states.bin"]),
        "laneInterleaveDigest": ArtifactDigest(files["mapping-contract.bin"]),
        "mappingStreamFrameSequenceZeroBlake3": stream_digest,
        "mappingStreamFormat": "BLAKE3(domain || repeated LE lane/carrier/plane/reserved/logical/tile records)",
        "independentRebuild": "tests/PBModulation/generate_unified_lc4_mapping_golden.py --check",
        "files": inventory,
    }
    files["manifest.json"] = PrettyJson(golden_manifest)
    WriteNewDirectory(output_directory, files)
    print(f"UNIFIED_LC4_GOLDEN_PASS files={len(files)} mapping_stream_blake3={stream_digest}")


def Main() -> None:
    parser = argparse.ArgumentParser(description="Deterministic provider-neutral Unified LC4 codebook/mapping search")
    parser.add_argument("--dataset", type=Path, required=True)
    subparsers = parser.add_subparsers(dest="command", required=True)
    select_parser = subparsers.add_parser("select")
    select_parser.add_argument("--output-directory", type=Path, required=True)
    holdout_parser = subparsers.add_parser("holdout")
    holdout_parser.add_argument("--selection-directory", type=Path, required=True)
    holdout_parser.add_argument("--output", type=Path, required=True)
    golden_parser = subparsers.add_parser("golden")
    golden_parser.add_argument("--selection-directory", type=Path, required=True)
    golden_parser.add_argument("--holdout-report", type=Path, required=True)
    golden_parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    manifest = LoadDataset(args.dataset)
    if args.command == "select":
        Select(manifest, args.dataset, args.output_directory)
    elif args.command == "holdout":
        EvaluateHoldout(manifest, args.selection_directory, args.output)
    else:
        BuildGolden(manifest, args.selection_directory, args.holdout_report, args.output_directory)


if __name__ == "__main__":
    Main()
