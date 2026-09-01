#!/usr/bin/env python3
"""Independent PB-RemoteVisual-LF4-X1 Golden oracle.

This generator reads no C++ header and invokes no PixelBridge executable. It
reuses only the independent Bootstrap RS/CRC scaffold and independently
transcribed Robust QC-LDPC oracle used by the older DesktopLevels Golden gate.
Generation is create-only; --check is strictly read-only.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import sys

from blake3 import blake3

sys.dont_write_bytecode = True
import generate_desktop_levels_golden as desktop_oracle
import generate_local_desktop_golden as scaffold


DEFAULT_ROOT = Path(__file__).resolve().parents[1] / "golden" / "remote-visual" / "lf4"
WIDTH, HEIGHT = 1920, 1080
PROFILE_ID = 0x504252564C463431
LAYOUT_VERSION = 7
SESSION_TAG = 0x5354455030325244
FRAME_SEQUENCE = 17
CONTROL_EPOCH = 0
DIAGNOSTIC_DOMAIN = b"PB-RemoteVisual-LF4-X1-Data"
TILE_PIXELS = 8
PHYSICAL_TILE_COUNT = 21456
DATA_TILE_COUNT = 16723
FRESHNESS_REGION_COLUMNS = 14
FRESHNESS_REGION_ROWS = 7
FRESHNESS_REGION_COUNT = FRESHNESS_REGION_COLUMNS * FRESHNESS_REGION_ROWS
MINIMUM_FRESHNESS_TAGS = 24
ELIGIBLE_FRESHNESS_REGIONS = 77
CODED_BITS_PER_PLANE = 16200
BITS_PER_TILE = 4
CODED_BITS = CODED_BITS_PER_PLANE * BITS_PER_TILE
DATA_BYTES = CODED_BITS // 8
CODEWORDS = 4
CODEWORD_BYTES = 2025
TRANSPORT_BYTES = 1350
PAYLOAD_BYTES = 1314
INTERLEAVE_MULTIPLIER = 65537
INTERLEAVE_INVERSE = 10330
INTERLEAVE_PHASE_STEP = 104
INTERLEAVE_PHASE_COUNT = 16
PLANE_SEQUENCE_OFFSETS = (0, 4, 8, 12)
ZERO_LUMA, ONE_LUMA, UNUSED_LUMA = 32, 224, 128
SYMBOL_MASKS = (
    0x3333, 0x00FF, 0xCC33, 0x9999, 0xF00F, 0x6699, 0x3CC3, 0x9669,
    0xCCCC, 0xFF00, 0x33CC, 0x6666, 0x0FF0, 0x9966, 0xC33C, 0x6996)
LADDERS = ((736, 16), (1696, 16), (96, 1000), (1056, 1000))
BANDS = ((96, 64, False), (160, 128, True), (288, 184, False),
         (476, 128, True), (608, 184, False), (792, 128, True),
         (920, 64, False))
METRIC_SCALE = 4096.0
METRIC_BINS = (
    (0.01, 0.0, 1669, 1450),
    (0.02, 0.2400000000000002, 0, 0),
    (0.04, 0.48000000000000004, 0, 0),
    (0.08, 0.9600000000000001, 0, 0),
    (0.12, 1.6, 0, 0),
    (0.2, 5.564520407322694, 130, 0),
    (0.35, 7.999755859375, 10466, 0),
    (0.5, 7.999755859375, 40695, 0),
    (0.75, 7.999755859375, 90435, 0),
    (1.0, 7.999755859375, 243054, 0),
    (1.5, 7.999755859375, 2351, 0),
    (2.0, 7.999755859375, 0, 0),
    (3.0, 7.999755859375, 0, 0),
    (4.0, 7.999755859375, 0, 0),
    (8.0, 7.999755859375, 0, 0),
    (1000000.0, 7.999755859375, 0, 0))
MASK64 = (1 << 64) - 1


def Float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def FloatBits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def BitsFloat(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def Digest(content: bytes) -> str:
    return blake3(content).hexdigest()


def Record() -> bytes:
    prefix = struct.pack("<4sBBBBQQQII", b"PBRG", 1, 1, 0, LAYOUT_VERSION,
                         PROFILE_ID, SESSION_TAG, FRAME_SEQUENCE, CONTROL_EPOCH, 0)
    assert len(prefix) == 40
    return prefix + struct.pack("<I", scaffold.Crc32C(prefix))


def TileRegions() -> list[tuple[int, int, int, int]]:
    regions = []
    for band_y, band_height, timing_corridors in BANDS:
        columns = 168 if timing_corridors else 216
        for row in range(band_height // TILE_PIXELS):
            for column in range(columns):
                if timing_corridors:
                    x = 224 + column * TILE_PIXELS if column < 84 else 1024 + (column - 84) * TILE_PIXELS
                else:
                    x = 96 + column * TILE_PIXELS
                regions.append((x, band_y + row * TILE_PIXELS, TILE_PIXELS, TILE_PIXELS))
    assert len(regions) == PHYSICAL_TILE_COUNT and len(set(regions)) == PHYSICAL_TILE_COUNT
    return regions


def RegionId(region: tuple[int, int, int, int]) -> int:
    x, y, _, _ = region
    return ((y - 96) // 128) * FRESHNESS_REGION_COLUMNS + (x - 96) // 128


def IsFreshnessCandidate(region: tuple[int, int, int, int]) -> bool:
    x, y, _, _ = region
    local_column = ((x - 96) // TILE_PIXELS) % 16
    local_row = ((y - 96) // TILE_PIXELS) % 16
    first_tag_column = (local_row * 5 + 1) % 16
    return local_column in (first_tag_column, (first_tag_column + 8) % 16)


def Mapping() -> list[tuple[int, int, int, int, int, int, int, int]]:
    regions = TileRegions()
    region_ids = [RegionId(region) for region in regions]
    assert all(0 <= region_id < FRESHNESS_REGION_COUNT for region_id in region_ids)
    tag_counts = [0] * FRESHNESS_REGION_COUNT
    for region, region_id in zip(regions, region_ids, strict=True):
        if IsFreshnessCandidate(region):
            tag_counts[region_id] += 1
    eligible = [count >= MINIMUM_FRESHNESS_TAGS for count in tag_counts]
    assert sum(eligible) == ELIGIBLE_FRESHNESS_REGIONS
    result = []
    data_ordinal = 0
    for physical, (region, region_id) in enumerate(zip(regions, region_ids, strict=True)):
        role = 0
        ordinal = DATA_TILE_COUNT
        if eligible[region_id]:
            if IsFreshnessCandidate(region):
                role = 1
            else:
                role = 2
                ordinal = data_ordinal
                data_ordinal += 1
        x, y, width, height = region
        result.append((physical, x, y, width, height, role, region_id, ordinal))
    assert data_ordinal == DATA_TILE_COUNT
    assert sum(entry[5] == 1 for entry in result) == 2389
    assert sum(entry[5] == 0 for entry in result) == 2344
    return result


def MappingBytes(mapping: list[tuple[int, int, int, int, int, int, int]]) -> bytes:
    records = bytearray()
    for physical, x, y, width, height, role, region_id, data_ordinal in mapping:
        records += struct.pack("<IHHBBBBHHI", physical, x, y, width, height, role, 0,
                               region_id, 0, data_ordinal)
    assert len(records) == PHYSICAL_TILE_COUNT * 20
    return struct.pack("<8sIII", b"PBLF4M01", 1, 20, PHYSICAL_TILE_COUNT) + records


def MetricCalibrationBytes() -> bytes:
    records = bytearray()
    for upper, calibrated, samples, errors in METRIC_BINS:
        records += struct.pack("<dI4xQQ", upper, FloatBits(calibrated), samples, errors)
    assert len(records) == len(METRIC_BINS) * 32
    return struct.pack("<8sIII", b"PBLF4C01", 1, 32, len(METRIC_BINS)) + records


def CalibrateMetric(raw_bits: int) -> tuple[bool, int, int, int]:
    raw = BitsFloat(raw_bits)
    magnitude = abs(float(raw))
    if not math.isfinite(raw) or magnitude > METRIC_BINS[-1][0]:
        return False, 0x7FC00000, 0, 0
    if raw == 0.0:
        calibrated = raw
    else:
        calibrated_magnitude = next(value for upper, value, _, _ in METRIC_BINS if magnitude <= upper)
        calibrated = math.copysign(Float32(calibrated_magnitude), raw)
    calibrated_bits = FloatBits(calibrated)
    scaled = max(-32767.0, min(32767.0, float(calibrated) * METRIC_SCALE))
    llr = math.floor(scaled + 0.5) if scaled >= 0 else math.ceil(scaled - 0.5)
    hard_bit = int(calibrated <= Float32(-0.5 / METRIC_SCALE))
    return True, calibrated_bits, int(llr), hard_bit


def MetricProbeBytes() -> tuple[bytes, int]:
    raw_bits = [0x00000000, 0x80000000, 0x00000001, 0x80000001]
    for upper, _, _, _ in METRIC_BINS:
        center = FloatBits(Float32(upper))
        assert center < 0x7F800000
        for bits in (center - 1, center, center + 1):
            raw_bits.extend((bits, bits | 0x80000000))
    raw_bits.extend((FloatBits(0.015), FloatBits(-0.015), FloatBits(0.1), FloatBits(-0.1),
                     FloatBits(0.3), FloatBits(-0.3), FloatBits(1.25), FloatBits(-1.25),
                     0x49742401, 0xC9742401, 0x7F800000, 0xFF800000, 0x7FC00001))
    raw_bits = list(dict.fromkeys(raw_bits))
    records = bytearray()
    for bits in raw_bits:
        valid, calibrated_bits, llr, hard_bit = CalibrateMetric(bits)
        records += struct.pack("<IBBHIhH", bits, int(valid), hard_bit, 0, calibrated_bits, llr, 0)
    assert len(records) == len(raw_bits) * 16
    return struct.pack("<8sIII", b"PBLF4P01", 1, 16, len(raw_bits)) + records, len(raw_bits)


def RotateLeft64(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (64 - shift))) & MASK64


def MixFreshness(value: int) -> int:
    value ^= value >> 30
    value = value * 0xBF58476D1CE4E5B9 & MASK64
    value ^= value >> 27
    value = value * 0x94D049BB133111EB & MASK64
    return (value ^ (value >> 31)) & MASK64


def FreshnessBit(physical: int, region_id: int) -> int:
    seed = SESSION_TAG ^ RotateLeft64(FRAME_SEQUENCE, 23)
    seed ^= (region_id + 1) * 0x9E3779B97F4A7C15 & MASK64
    seed ^= (physical + 1) * 0xD6E8FEB86659FD93 & MASK64
    return MixFreshness(seed) & 1


def LogicalBit(data_ordinal: int, plane: int) -> int:
    sequence = FRAME_SEQUENCE + PLANE_SEQUENCE_OFFSETS[plane]
    unshifted = (data_ordinal + DATA_TILE_COUNT - (sequence % INTERLEAVE_PHASE_COUNT) * INTERLEAVE_PHASE_STEP) % DATA_TILE_COUNT
    logical = unshifted * INTERLEAVE_INVERSE % DATA_TILE_COUNT
    return plane * CODED_BITS_PER_PLANE + logical if logical < CODED_BITS_PER_PLANE else CODED_BITS


def Raster(record: bytes, coded_data: bytes,
           mapping: list[tuple[int, int, int, int, int, int, int, int]]) -> bytes:
    pixels = bytearray(scaffold.Raster(scaffold.EncodeRs(record), scaffold.TimingBits(record), scaffold.MarkerBits()))

    def Fill(x: int, y: int, width: int, height: int, level: int) -> None:
        assert 0 <= x < x + width <= WIDTH and 0 <= y < y + height <= HEIGHT
        row = bytes((level, level, level, 255)) * width
        for offset_y in range(y, y + height):
            offset = (offset_y * WIDTH + x) * 4
            pixels[offset:offset + len(row)] = row

    for x, y in LADDERS:
        for level in range(4):
            Fill(x + level * 32, y, 32, 64, 32 + level * 64)
    for physical, x, y, width, height, role, region_id, data_ordinal in mapping:
        if role == 0:
            Fill(x, y, width, height, UNUSED_LUMA)
        elif role == 1:
            Fill(x, y, width, height, ONE_LUMA if FreshnessBit(physical, region_id) else ZERO_LUMA)
        else:
            symbol = 0
            for plane in range(BITS_PER_TILE):
                logical = LogicalBit(data_ordinal, plane)
                if logical < CODED_BITS and coded_data[logical // 8] & (1 << (logical % 8)):
                    symbol |= 1 << plane
            mask = SYMBOL_MASKS[symbol]
            for chip in range(16):
                Fill(x + chip % 4 * 2, y + chip // 4 * 2, 2, 2,
                     ONE_LUMA if mask & (1 << chip) else ZERO_LUMA)
    assert len(pixels) == WIDTH * HEIGHT * 4
    return bytes(pixels)


def BuildFiles() -> tuple[dict[str, bytes], bytes]:
    assert blake3(b"").hexdigest() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
    assert DATA_BYTES == CODEWORDS * CODEWORD_BYTES and CODED_BITS == DATA_BYTES * 8
    assert INTERLEAVE_MULTIPLIER * INTERLEAVE_INVERSE % DATA_TILE_COUNT == 1
    assert all(mask.bit_count() == 8 for mask in SYMBOL_MASKS)
    assert all((SYMBOL_MASKS[index] ^ SYMBOL_MASKS[index + 8]) == 0xFFFF for index in range(8))
    assert min((left ^ right).bit_count() for index, left in enumerate(SYMBOL_MASKS)
               for right in SYMBOL_MASKS[index + 1:]) == 8
    record = Record()
    coded_data = desktop_oracle.DiagnosticData(record, DATA_BYTES, DIAGNOSTIC_DOMAIN)
    assert len(record) == 44 and len(coded_data) == DATA_BYTES
    mapping = Mapping()
    mapping_bytes = MappingBytes(mapping)
    codebook_bytes = struct.pack("<16H", *SYMBOL_MASKS)
    metric_calibration = MetricCalibrationBytes()
    metric_probes, metric_probe_count = MetricProbeBytes()
    raster = Raster(record, coded_data, mapping)
    raster_digest = Digest(raster)
    assert raster_digest == "28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4"
    files = {
        "lf4-bootstrap.bin": record,
        "lf4-coded-data.bin": coded_data,
        "lf4-codebook.bin": codebook_bytes,
        "lf4-mapping.bin": mapping_bytes,
        "lf4-metric-calibration.bin": metric_calibration,
        "lf4-metric-probes.bin": metric_probes,
        "lf4-raster.blake3": (raster_digest + "\n").encode("ascii")}
    accepted = []
    for slot in range(CODEWORDS):
        transport = coded_data[slot * CODEWORD_BYTES:slot * CODEWORD_BYTES + TRANSPORT_BYTES]
        assert len(transport) == TRANSPORT_BYTES
        name = f"lf4-accepted-transport-{slot}.bin"
        files[name] = transport
        accepted.append({"slot": slot, "file": name, "bytes": len(transport), "blake3": Digest(transport)})
    file_inventory = [{"file": name, "bytes": len(content), "blake3": Digest(content)}
                      for name, content in sorted(files.items())]
    manifest = {
        "schema": "PixelBridge.RemoteVisualLowFpsGolden.1",
        "status": "ExperimentalGoldenNotCertifiedProfile",
        "wire": {
            "profileIdHex": f"{PROFILE_ID:016x}",
            "layoutVersion": LAYOUT_VERSION,
            "bootstrapContract": "PB-Bootstrap-1",
            "transportContract": "PB-TransportBlock-1",
            "innerFec": "Robust DVB-S2 Short QC-LDPC",
            "bootstrapFile": "lf4-bootstrap.bin",
            "bootstrapBlake3": Digest(record),
            "sessionTagHex": f"{SESSION_TAG:016x}",
            "frameSequence": FRAME_SEQUENCE,
            "controlEpoch": CONTROL_EPOCH,
            "diagnosticPayloadDomain": DIAGNOSTIC_DOMAIN.decode("ascii")},
        "raster": {
            "width": WIDTH,
            "height": HEIGHT,
            "pixelFormat": "BGRA8",
            "bytes": len(raster),
            "stored": False,
            "rebuild": "generate_remote_visual_lf4_golden.py --output-dir <new-fixture-directory> --raster-output <new-file>",
            "blake3": raster_digest},
        "mapping": {
            "file": "lf4-mapping.bin",
            "format": "PBLF4M01/LE/v1/20-byte-record",
            "tilePixels": TILE_PIXELS,
            "physicalTileCount": PHYSICAL_TILE_COUNT,
            "unusedTiles": 2344,
            "freshnessTagTiles": 2389,
            "dataTileCount": DATA_TILE_COUNT,
            "freshnessRegionGrid": [FRESHNESS_REGION_COLUMNS, FRESHNESS_REGION_ROWS],
            "minimumFreshnessTags": MINIMUM_FRESHNESS_TAGS,
            "eligibleFreshnessRegions": ELIGIBLE_FRESHNESS_REGIONS,
            "interleaveMultiplier": INTERLEAVE_MULTIPLIER,
            "interleaveInverse": INTERLEAVE_INVERSE,
            "interleavePhaseStep": INTERLEAVE_PHASE_STEP,
            "interleavePhaseCount": INTERLEAVE_PHASE_COUNT,
            "planeSequenceOffsets": list(PLANE_SEQUENCE_OFFSETS)},
        "codebook": {
            "file": "lf4-codebook.bin",
            "format": "16xLE16",
            "bitsPerTile": BITS_PER_TILE,
            "chipsPerSymbol": 16,
            "minimumHammingDistance": 8,
            "masksHex": [f"{mask:04x}" for mask in SYMBOL_MASKS]},
        "codedData": {
            "file": "lf4-coded-data.bin",
            "bytes": len(coded_data),
            "codedBitsPerPlane": CODED_BITS_PER_PLANE,
            "codedBits": CODED_BITS,
            "codewords": CODEWORDS,
            "codewordBytes": CODEWORD_BYTES,
            "paddingBytes": 0,
            "blake3": Digest(coded_data)},
        "metricCalibration": {
            "candidateId": "lf4-default/PiecewiseLookup",
            "disposition": "FrozenCandidateNotProductionDefault",
            "sourceReportSha256": "c17f17844c0b44cf029bb31ecbfc127139fa886588c941c2a3a0f34bf541acf8",
            "sourceCorePayloadBlake3": "7a8ff14e09dc5f0b4417349f9ab3053f0e0dac05312be94806e55d40324f619f",
            "calibrationFile": "lf4-metric-calibration.bin",
            "calibrationFormat": "PBLF4C01/LE/v1/32-byte-record",
            "rawInput": "IEEE754-binary32",
            "signRule": "preserve-sign-including-negative-zero",
            "terminalRawMagnitudeInclusive": METRIC_BINS[-1][0],
            "nonfiniteOrAboveTerminal": "reject-without-output-mutation",
            "adapter": {
                "scale": METRIC_SCALE,
                "clampInclusive": [-32767, 32767],
                "rounding": "C++ std::round half away from zero",
                "output": "signed little-endian int16",
                "hardOneDecision": "calibratedMetric <= float32(-0.5/4096)"},
            "probeFile": "lf4-metric-probes.bin",
            "probeFormat": "PBLF4P01/LE/v1/16-byte-record",
            "probeCount": metric_probe_count},
        "acceptedTransport": accepted,
        "truthBoundary": {
            "senderTruthEntersDemodulation": False,
            "senderTruthEntersFec": False,
            "postAdmissionExactByteComparison": True,
            "wholeFileOutputEvaluated": False,
            "certifiedProfile": False},
        "files": file_inventory}
    files["manifest.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    return files, raster


def CheckFiles(root: Path, files: dict[str, bytes]) -> None:
    if not root.is_dir() or root.is_symlink():
        raise SystemExit(f"Golden directory missing or unsafe: {root}")
    actual_names = {path.name for path in root.iterdir()}
    if actual_names != set(files):
        raise SystemExit(f"Golden inventory mismatch: expected={sorted(files)} actual={sorted(actual_names)}")
    for name, content in files.items():
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise SystemExit(f"Golden entry is not a regular file: {path}")
        if path.read_bytes() != content:
            raise SystemExit(f"Golden mismatch, never re-pin: {path}")


def WriteFiles(root: Path, files: dict[str, bytes]) -> None:
    if root.exists():
        raise SystemExit(f"Refusing existing output directory: {root}")
    root.mkdir(parents=True)
    for name, content in files.items():
        with (root / name).open("xb") as output:
            output.write(content)


def Main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--raster-output", type=Path)
    args = parser.parse_args()
    if args.check and args.raster_output is not None:
        parser.error("--check is read-only and cannot be combined with --raster-output")
    if args.raster_output is not None and args.raster_output.exists():
        raise SystemExit(f"Refusing existing raster output: {args.raster_output}")
    files, raster = BuildFiles()
    if args.check:
        CheckFiles(args.output_dir, files)
    else:
        WriteFiles(args.output_dir, files)
    if args.raster_output is not None:
        args.raster_output.parent.mkdir(parents=True, exist_ok=True)
        with args.raster_output.open("xb") as output:
            output.write(scaffold.Pbrw(raster))
    print(f"REMOTE_VISUAL_LF4_GOLDEN_PASS files={len(files)} raster_blake3={Digest(raster)}")


if __name__ == "__main__":
    Main()
