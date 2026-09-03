from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys
from typing import Any

from blake3 import blake3


sys.dont_write_bytecode = True

SCHEMA = "PixelBridge.UnifiedLc4DatasetManifest.1"
MANIFEST_VERSION = 1
SPLIT_DOMAIN = b"PixelBridge.UnifiedLc4DatasetSplit.1\0"
SAMPLE_DOMAIN = b"PixelBridge.UnifiedLc4DatasetSample.1\0"
SOURCE_DOMAIN = b"PixelBridge.UnifiedLc4HistoricalSource.1\0"
SPLIT_SEED_HEX = "7d21f1c9a0b45e33d6c803b27194fa80"

LF4_MASKS = (
    0x3333, 0x00FF, 0xCC33, 0x9999, 0xF00F, 0x6699, 0x3CC3, 0x9669,
    0xCCCC, 0xFF00, 0x33CC, 0x6666, 0x0FF0, 0x9966, 0xC33C, 0x6996)
SHAPE_MASKS = (
    0x00FF, 0xFF00, 0x3333, 0xCCCC, 0x0FF0, 0xF00F, 0x6666, 0x9999,
    0x7331, 0x8CCE, 0x1337, 0xECC8, 0x8C73, 0x738C, 0x13EC, 0xEC13)
CHROMA_STATES = (
    (-24, 7, -16, 0),
    (24, 2, -16, 1),
    (24, -7, 16, 3),
    (-24, -2, 16, 2))
SCALES = ((3, 4), (17, 20), (1, 1), (3, 2), (2, 1))

VARIANTS: tuple[dict[str, Any], ...] = (
    {
        "id": "clean",
        "filter": "area",
        "originQ8": [0, 0],
        "letterbox": False,
        "gaussianBlurPasses": 0,
        "quantizationBitsPerChannel": 8,
        "chroma420": False,
        "neutralChroma": False,
    },
    {
        "id": "fractional-a",
        "filter": "bilinear",
        "originQ8": [64, -96],
        "letterbox": False,
        "gaussianBlurPasses": 0,
        "quantizationBitsPerChannel": 8,
        "chroma420": False,
        "neutralChroma": False,
    },
    {
        "id": "fractional-b-letterbox",
        "filter": "bicubic-catmull-rom-q16",
        "originQ8": [-96, 32],
        "letterbox": True,
        "gaussianBlurPasses": 0,
        "quantizationBitsPerChannel": 8,
        "chroma420": False,
        "neutralChroma": False,
    },
    {
        "id": "blur-quant6",
        "filter": "area",
        "originQ8": [0, 0],
        "letterbox": False,
        "gaussianBlurPasses": 1,
        "quantizationBitsPerChannel": 6,
        "chroma420": False,
        "neutralChroma": False,
    },
    {
        "id": "chroma420-quant5",
        "filter": "bilinear",
        "originQ8": [32, 64],
        "letterbox": False,
        "gaussianBlurPasses": 0,
        "quantizationBitsPerChannel": 5,
        "chroma420": True,
        "neutralChroma": False,
    },
    {
        "id": "neutral-chroma-blur-quant5",
        "filter": "bilinear",
        "originQ8": [-64, -32],
        "letterbox": True,
        "gaussianBlurPasses": 1,
        "quantizationBitsPerChannel": 5,
        "chroma420": False,
        "neutralChroma": True,
    })


def CanonicalJson(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")


def PrettyJson(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def Digest(domain: bytes, content: bytes) -> str:
    hasher = blake3()
    hasher.update(domain)
    hasher.update(content)
    return hasher.hexdigest()


def SourceDigest(masks: tuple[int, ...], low_luma: int, high_luma: int,
                 chroma_states: tuple[tuple[int, int, int, int], ...]) -> str:
    content = struct.pack("<16HBB", *masks, low_luma, high_luma)
    content += b"".join(struct.pack("<hhhB", *state) for state in chroma_states)
    return Digest(SOURCE_DOMAIN, content)


def HistoricalSources() -> list[dict[str, Any]]:
    return [
        {
            "id": "historical-lf4",
            "origin": "pbmodulation::kRemoteVisualLowFpsSymbolMasks",
            "maskBitOrder": "row-major-4x4-lsb-first",
            "masksHexByHistoricalLabel": [f"{mask:04x}" for mask in LF4_MASKS],
            "lowLuma": 32,
            "highLuma": 224,
            "chromaStates": [list(state) for state in CHROMA_STATES],
            "sourceBlake3": SourceDigest(LF4_MASKS, 32, 224, CHROMA_STATES),
        },
        {
            "id": "historical-shape-chroma",
            "origin": "pbmodulation::kShapeChromaTemplates/kShapeChromaStates",
            "maskBitOrder": "row-major-4x4-lsb-first",
            "masksHexByHistoricalLabel": [f"{mask:04x}" for mask in SHAPE_MASKS],
            "lowLuma": 80,
            "highLuma": 176,
            "chromaStates": [list(state) for state in CHROMA_STATES],
            "sourceBlake3": SourceDigest(SHAPE_MASKS, 80, 176, CHROMA_STATES),
        }]


def SampleDescriptor(source: dict[str, Any], scale: tuple[int, int], variant: dict[str, Any]) -> dict[str, Any]:
    numerator, denominator = scale
    return {
        "sampleId": f"{source['id']}-scale-{numerator}-{denominator}-{variant['id']}",
        "sourceId": source["id"],
        "sourceBlake3": source["sourceBlake3"],
        "scale": {"numerator": numerator, "denominator": denominator},
        "transforms": {
            "resampleFilter": variant["filter"],
            "originQ8": variant["originQ8"],
            "centeredLetterbox": variant["letterbox"],
            "gaussianBlurPasses": variant["gaussianBlurPasses"],
            "quantizationBitsPerChannel": variant["quantizationBitsPerChannel"],
            "chroma420": variant["chroma420"],
            "neutralChroma": variant["neutralChroma"],
        },
        "spatialQualityModel": {
            "schema": "PixelBridge.UnifiedLc4SpatialQualityQ12.1",
            "periodTiles": [17, 13],
            "phaseSource": "sampleBlake3",
            "minimumQualityQ12": 2560,
        },
    }


def BuildManifest() -> dict[str, Any]:
    sources = HistoricalSources()
    split_seed = bytes.fromhex(SPLIT_SEED_HEX)
    samples: list[dict[str, Any]] = []
    for source in sources:
        for scale in SCALES:
            descriptors = [SampleDescriptor(source, scale, variant) for variant in VARIANTS]
            ranked = sorted(descriptors, key=lambda descriptor: (
                Digest(SPLIT_DOMAIN, split_seed + CanonicalJson(descriptor)), descriptor["sampleId"]))
            for rank, descriptor in enumerate(ranked):
                split = "Train" if rank < 3 else "Validation" if rank < 5 else "Holdout"
                descriptor = dict(descriptor)
                descriptor["sampleBlake3"] = Digest(SAMPLE_DOMAIN, CanonicalJson(descriptor))
                descriptor["split"] = split
                descriptor["splitRankWithinStratum"] = rank
                samples.append(descriptor)
    samples.sort(key=lambda sample: sample["sampleId"])
    counts = {split: sum(sample["split"] == split for sample in samples)
              for split in ("Train", "Validation", "Holdout")}
    core = {
        "schema": SCHEMA,
        "version": MANIFEST_VERSION,
        "status": "SealedBeforeSearch",
        "profile": {
            "name": "PB-Unified-LC4-V1",
            "profileIdHex": "5042554e494c4331",
            "layoutVersion": 8,
            "tilePixels": 4,
            "dataTiles": 86688,
        },
        "simulationModel": {
            "schema": "PixelBridge.UnifiedLc4TileChannelQ12.1",
            "arithmetic": "signed-int64/Q12/round-half-away-from-zero",
            "neutralChroma": "BT.709 integer luma (54R+183G+19B+128)/256",
            "chroma420": "centered 2x2 average of luma-relative B/R deltas",
            "quantization": "uniform round-nearest B/G/R; alpha excluded",
        },
        "historicalSources": sources,
        "requiredScales": [
            {"numerator": numerator, "denominator": denominator,
             "decimal": f"{numerator / denominator:.2f}"}
            for numerator, denominator in SCALES],
        "splitAlgorithm": {
            "schema": "PixelBridge.UnifiedLc4StratifiedBlake3Split.1",
            "seedHex": SPLIT_SEED_HEX,
            "strata": ["sourceId", "scale.numerator", "scale.denominator"],
            "ordering": "BLAKE3(domain || seed || canonical sample descriptor), then sampleId",
            "assignmentByRank": {"Train": [0, 1, 2], "Validation": [3, 4], "Holdout": [5]},
            "holdoutRule": "Descriptors are sealed here; outcomes are not evaluated during selection.",
        },
        "sampleCounts": counts,
        "samples": samples,
    }
    return {**core, "manifestBlake3": Digest(b"PixelBridge.UnifiedLc4DatasetManifestCore.1\0", CanonicalJson(core))}


def ValidateManifest(manifest: dict[str, Any]) -> None:
    expected = BuildManifest()
    if manifest != expected:
        raise SystemExit("Dataset manifest mismatch; never re-split after search")


def WriteNew(path: Path, content: bytes) -> None:
    if path.exists():
        raise SystemExit(f"Refusing existing output file: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(content)


def Main() -> None:
    parser = argparse.ArgumentParser(description="Seal the provider-neutral Unified LC4 search dataset")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    expected = BuildManifest()
    if args.check:
        if not args.output.is_file() or args.output.is_symlink():
            raise SystemExit(f"Dataset manifest missing or unsafe: {args.output}")
        actual = json.loads(args.output.read_text(encoding="utf-8"))
        ValidateManifest(actual)
    else:
        WriteNew(args.output, PrettyJson(expected))
    print(f"UNIFIED_LC4_DATASET_PASS samples={len(expected['samples'])} "
          f"manifest_blake3={expected['manifestBlake3']}")


if __name__ == "__main__":
    Main()
